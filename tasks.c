/* build via 
 *  gcc -g tasks.c -o tasks `pkg-config --libs --cflags x11 gtk+-2.0` 
 */

#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkevents.h>

#include <gtk/gtk.h>

#define TN_WANT_DEBUG 1

#if (TN_WANT_DEBUG)
#define TN_DBG(x, a...) \
 fprintf(stderr, __FILE__ ":%d,%s() " x "\n", __LINE__, __func__, ##a)
#else
#define TN_DBG(x, a...) do {} while (0)
#endif
#define TN_MARK() TN_DBG("--mark--");

enum
{
  TN_ATOM_WM_CLASS,
  TN_ATOM_WM_NAME,
  TN_ATOM_WM_STATE,
  TN_ATOM_WM_TRANSIENT_FOR,

  TN_ATOM_NET_WM_WINDOW_TYPE,
  TN_ATOM_NET_WM_WINDOW_TYPE_MENU,
  TN_ATOM_NET_WM_WINDOW_TYPE_NORMAL,
  TN_ATOM_NET_WM_WINDOW_TYPE_DIALOG,
  TN_ATOM_NET_WM_WINDOW_TYPE_DESKTOP,
  TN_ATOM_NET_WM_STATE,
  TN_ATOM_NET_WM_STATE_MODAL,
  TN_ATOM_NET_SHOWING_DESKTOP,
  TN_ATOM_NET_WM_PID,
  TN_ATOM_NET_ACTIVE_WINDOW,
  TN_ATOM_NET_CLIENT_LIST,

  TN_ATOM_HILDON_APP_KILLABLE,

  TN_ATOM_MB_WIN_SUB_NAME,
  TN_ATOM_MB_COMMAND,
  TN_ATOM_MB_CURRENT_APP_WINDOW,

  TN_ATOM_UTF8_STRING,

  TN_ATOM_COUNT
};

/* FIXME: remove globals */
Atom        Atoms[TN_ATOM_COUNT];
GHashTable *Windows; 

typedef struct TNWMWindow
{
  Window   xwin;
  gchar   *name;
  gchar   *bin_name;
  gchar   *class_name;
  Window   xtransient;

  gboolean show_startup_banner;
  gboolean is_killed;
  gboolean is_minimised;

} TNWMWindow;

struct xwinv
{
  Window *wins;
  gint    n_wins;
};

#define TN_WM_SYNC_NAME       (1<<1)
#define TN_WM_SYNC_BIN_NAME   (1<<2)
#define TN_WM_SYNC_CLASS_NAME (1<<3)
#define TN_WM_SYNC_ALL        (G_MAXULONG)

TNWMWindow*
tn_wm_window_add (Window xid);

static void
tn_wm_window_destroy (TNWMWindow *win);

static gboolean
tn_wm_window_props_sync (TNWMWindow *win, gulong props);

static void 
tn_wm_process_client_list(void);

static GdkFilterReturn 
tn_wm_x_event_filter (GdkXEvent *xevent, 
		      GdkEvent  *event, 
		      gpointer   data);


static void
tn_wm_atoms_init()
{
  /*  
   *   The list below *MUST* be kept in the same order as the corresponding
   *   emun above or *everything* will break.
   *   Doing it like this avoids a mass of round trips on startup.
   */

  char *atom_names[] = {
    
    "WM_CLASS",			/* ICCCM */
    "WM_NAME",
    "WM_STATE",
    "WM_TRANSIENT_FOR",

    "_NET_WM_WINDOW_TYPE",	/* EWMH */
    "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_STATE",
    "_NET_WM_STATE_MODAL",
    "_NET_SHOWING_DESKTOP",
    "_NET_WM_PID",
    "_NET_ACTIVE_WINDOW",
    "_NET_CLIENT_LIST",

    "_HILDON_APP_KILLABLE",	/* Hildon only props */

    "_MB_WIN_SUB_NAME",		/* MB Only props */
    "_MB_COMMAND",              /* FIXME: Unused */
    "_MB_CURRENT_APP_WINDOW",

    "UTF8_STRING",
  };

  /* FIXME: Error Traps */

  XInternAtoms (GDK_DISPLAY(), 
		atom_names, 
		TN_ATOM_COUNT,
                False, 
		Atoms);
}

static void*
tn_wm_get_win_prop_data_and_validate (Window     xwin, 
				      Atom       prop, 
				      Atom       type, 
				      int        expected_format,
				      int        expected_n_items,
				      int       *n_items_ret)
{
  Atom           type_ret;
  int            format_ret;
  unsigned long  items_ret;
  unsigned long  after_ret;
  unsigned char *prop_data;
  int            status;

  prop_data = NULL;
  
  gdk_error_trap_push();

  status = XGetWindowProperty (GDK_DISPLAY(), 
			       xwin, 
			       prop, 
			       0, G_MAXLONG, 
			       False,
			       type, 
			       &type_ret, 
			       &format_ret, 
			       &items_ret,
			       &after_ret, 
			       &prop_data);


  if (gdk_error_trap_pop() || status != Success || prop_data == NULL)
    goto fail;

  if (expected_format && format_ret != expected_format)
    goto fail;

  if (expected_n_items && items_ret != expected_n_items)
    goto fail;

  if (n_items_ret)
    *n_items_ret = items_ret;
  
  return prop_data;

 fail:

  if (prop_data)
    XFree(prop_data);

  return NULL;
}

TNWMWindow*
tn_wm_window_add (Window xid)
{
  TNWMWindow *win = NULL;
  GdkWindow  *gdk_wrapper_win = NULL;
  gint       *key;

  win = g_new0 (TNWMWindow, 1);  

  if (!win) 	 /* FIXME: Handle OOM */
    return NULL;

  key = g_new (gint, 1);

  if (!key) 	 /* FIXME: Handle OOM */
    return NULL;

  win->xwin = *key = xid;

  if (!tn_wm_window_props_sync (win, TN_WM_SYNC_ALL))
    goto abort;

  gdk_error_trap_push();
            
  gdk_wrapper_win = gdk_window_foreign_new (xid);
            
  if (gdk_wrapper_win != NULL)
    {
      /* Monitor the window for prop changes */
      gdk_window_set_events(gdk_wrapper_win,
			    gdk_window_get_events(gdk_wrapper_win)
			    | GDK_PROPERTY_CHANGE_MASK);

      gdk_window_add_filter(gdk_wrapper_win, 
			    tn_wm_x_event_filter,
			    NULL);
    }        
     
  XSync(GDK_DISPLAY(), False);  /* FIXME: Check above does not sync */

  if (gdk_error_trap_pop() || gdk_wrapper_win == NULL)
    goto abort;

  g_object_unref(gdk_wrapper_win);

  g_hash_table_insert (Windows, key, (gpointer)win);

  return win;

 abort:

  TN_DBG(" *** Aborting on XID: 0x%lx ***\n", xid);

  if (win)
    tn_wm_window_destroy (win);

  if (gdk_wrapper_win)
    g_object_unref(gdk_wrapper_win);

  return NULL;
}

static void
tn_wm_window_destroy (TNWMWindow *win)
{
  if (win->name) XFree(win->name);
  g_free(win);
}


static gboolean
tn_wm_window_props_sync (TNWMWindow *win, gulong props)
{
  if (props & TN_WM_SYNC_NAME)
    {
      if (win->name) 
	XFree(win->name);

      /* FIXME: handle UTF8 naming and trap */
      XFetchName(GDK_DISPLAY(), win->xwin, &win->name);
    }

  return TRUE;
}

static gboolean 
client_list_remove_foreach_func (gpointer key,
				 gpointer value,
				 gpointer userdata)
{
  TNWMWindow   *win;
  struct xwinv *xwins;
  int    i;

  xwins = (struct xwinv*)userdata;
  win   = (TNWMWindow *)value;

  for (i=0; i < xwins->n_wins; i++)
    if (G_UNLIKELY((xwins->wins[i] == win->xwin)))
	return FALSE;

  /* FIXME: maybe want to query window xid here to check its really gone */

  printf("@@ Removing Window: Xid: 0x%lx, Name: '%s'\n", 
	 win->xwin,
	 win->name ? win->name : "Unknown" );
  
  return TRUE;
}

static void 
tn_wm_process_client_list(void)
{
  struct xwinv xwins;
  int     i;

  xwins.wins 
    = tn_wm_get_win_prop_data_and_validate (GDK_ROOT_WINDOW(),
					    Atoms[TN_ATOM_NET_CLIENT_LIST],
					    XA_WINDOW,
					    32,
					    0,
					    &xwins.n_wins);
  if (G_UNLIKELY(xwins.wins == NULL))
    return;

  /* Check if any windows in our hash have since dissapeared */
  g_hash_table_foreach_remove ( Windows, 
				client_list_remove_foreach_func,
				(gpointer)&xwins);

  /* Now add any new ones  */
  for (i=0; i < xwins.n_wins; i++)
    {
      if (!g_hash_table_lookup(Windows, (gconstpointer)&xwins.wins[i]))
	{
	  TNWMWindow *win;

	  win = tn_wm_window_add (xwins.wins[i]);

	  if (win)
	    {
	      printf("@@ Adding Window: Xid: 0x%lx, Name: '%s'\n", 
		     win->xwin,
		     win->name ? win->name : "Unknown" );
	    }
	}
    }
}

static GdkFilterReturn 
tn_wm_x_event_filter (GdkXEvent *xevent, 
		      GdkEvent  *event, 
		      gpointer   data)
{
  XPropertyEvent *prop;
  TNWMWindow     *win;    

  /* If this isn't a property change event ignore ASAP */
  if (((XEvent*)xevent)->type != PropertyNotify)
    return GDK_FILTER_CONTINUE;
  
  prop = (XPropertyEvent*)xevent;

  /* Root window property change */
  if (G_LIKELY(prop->window == GDK_ROOT_WINDOW()))
    {
      if (prop->atom == Atoms[TN_ATOM_NET_CLIENT_LIST])
	{
	  tn_wm_process_client_list();
	}
      else if (prop->atom == Atoms[TN_ATOM_NET_ACTIVE_WINDOW])
	{
	  printf("## Active window changed?\n");
	}
      else if (prop->atom == Atoms[TN_ATOM_NET_SHOWING_DESKTOP])
	{
	  printf("## Desktop showing state toggled?\n");
	}
      else if (prop->atom == Atoms[TN_ATOM_MB_CURRENT_APP_WINDOW])
	{
	  printf("## Matchbox 'Topped' App changed?\n");
	}
    }
  else
    {
      /* Hopefully one of our watched windows changing a prop..
       * 
       * Check if its an atom were actually interested in	 
       * before doing the assumed to be more expensive hash 
       * lookup.
      */
      
      if ( prop->atom == Atoms[TN_ATOM_WM_CLASS]
	   || prop->atom == Atoms[TN_ATOM_WM_NAME]
	   || prop->atom == Atoms[TN_ATOM_WM_STATE]
	   || prop->atom == Atoms[TN_ATOM_WM_TRANSIENT_FOR]
	   || prop->atom == Atoms[TN_ATOM_NET_WM_STATE])
	{
	  win = g_hash_table_lookup(Windows, (gconstpointer)&prop->window);
	}

      if (!win) 
	return GDK_FILTER_CONTINUE;

      if (prop->atom == Atoms[TN_ATOM_WM_NAME])
	{
	  printf("## Window '%s' ( 0x%lx ) changed name to ",
		 win->name ? win->name : "Unkown", win->xwin );
	  tn_wm_window_props_sync (win, TN_WM_SYNC_NAME);
	  printf("'%s'\n", win->name ? win->name : "Unkown");
	}
      else if (prop->atom == Atoms[TN_ATOM_WM_CLASS])
	{
	  printf("## Window '%s' ( 0x%lx ) changed class.\n",
		 win->name ? win->name : "Unkown", win->xwin );
	}
      else if (prop->atom == Atoms[TN_ATOM_WM_STATE])
	{
	  printf("## Window '%s' ( 0x%lx ) changed ICCCM state.\n",
		 win->name ? win->name : "Unkown", win->xwin );

	}
      else if (prop->atom == Atoms[TN_ATOM_WM_TRANSIENT_FOR])
	{
	  printf("## Window '%s' ( 0x%lx ) changed transiency.\n",
		 win->name ? win->name : "Unkown", win->xwin );

	}
      else if (prop->atom == Atoms[TN_ATOM_WM_STATE])
	{
	  printf("## Window '%s' ( 0x%lx ) changed EWMH state.\n",
		 win->name ? win->name : "Unkown", win->xwin );
	}
    }

  return GDK_FILTER_CONTINUE;
}

int 
main(int argc, char**argv)
{
  gtk_init(&argc, &argv);

  tn_wm_atoms_init();

  /* Hash to track watched windows */
  Windows = g_hash_table_new_full (g_int_hash, 
				   g_int_equal,
				   (GDestroyNotify)g_free,
				   (GDestroyNotify)tn_wm_window_destroy);
  gdk_error_trap_push();
    
  gdk_window_set_events(gdk_get_default_root_window(),
			gdk_window_get_events(gdk_get_default_root_window())
                        | GDK_PROPERTY_CHANGE_MASK );
  
  gdk_window_add_filter(gdk_get_default_root_window(), 
			tn_wm_x_event_filter,
			NULL);
    
  gdk_error_trap_pop();

  /* Populate existing windows */
  tn_wm_process_client_list();

  gtk_main();

  return 0;
}
