/*****************************************************************************
 * intf_gtk.c: Gtk+ interface
 *****************************************************************************
 * Copyright (C) 1999, 2000 VideoLAN
 * $Id: intf_gtk.c,v 1.2 2001/03/04 03:12:00 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#define MODULE_NAME gtk
#include "modules_inner.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <string.h>                                            /* strerror() */
#include <stdio.h>

#include <gtk/gtk.h>

#include "config.h"
#include "common.h"
#include "threads.h"
#include "mtime.h"
#include "tests.h"
#include "modules.h"

#include "stream_control.h"
#include "input_ext-intf.h"

#include "intf_msg.h"
#include "interface.h"

#include "gtk_sys.h"
#include "gtk_interface.h"
#include "gtk_support.h"

#include "main.h"

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/
static int  intf_Probe     ( probedata_t *p_data );
static int  intf_Open      ( intf_thread_t *p_intf );
static void intf_Close     ( intf_thread_t *p_intf );
static void intf_Run       ( intf_thread_t *p_intf );

static gint GtkManage    ( gpointer p_data );

/*****************************************************************************
 * g_atexit: kludge to avoid the Gtk+ thread to segfault at exit
 *****************************************************************************
 * gtk_init() makes several calls to g_atexit() which calls atexit() to
 * register tidying callbacks to be called at program exit. Since the Gtk+
 * plugin is likely to be unloaded at program exit, we have to export this
 * symbol to intercept the g_atexit() calls. Talk about crude hack.
 *****************************************************************************/
void g_atexit( GVoidFunc func )
{
    intf_thread_t *p_intf = p_main->p_intf;

    if( p_intf->p_sys->pf_gdk_callback == NULL )
    {
        p_intf->p_sys->pf_gdk_callback = func;
    }
    else if( p_intf->p_sys->pf_gtk_callback == NULL )
    {
        p_intf->p_sys->pf_gtk_callback = func;
    }
    /* else nothing, but we could do something here */
    return;
}

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
void _M( intf_getfunctions )( function_list_t * p_function_list )
{
    p_function_list->pf_probe = intf_Probe;
    p_function_list->functions.intf.pf_open  = intf_Open;
    p_function_list->functions.intf.pf_close = intf_Close;
    p_function_list->functions.intf.pf_run   = intf_Run;
}

/*****************************************************************************
 * intf_Probe: probe the interface and return a score
 *****************************************************************************
 * This function tries to initialize Gtk+ and returns a score to the
 * plugin manager so that it can select the best plugin.
 *****************************************************************************/
static int intf_Probe( probedata_t *p_data )
{
    if( TestMethod( INTF_METHOD_VAR, "gtk" ) )
    {
        return( 999 );
    }

    return( 90 );
}

/*****************************************************************************
 * intf_Open: initialize and create window
 *****************************************************************************/
static int intf_Open( intf_thread_t *p_intf )
{
    /* Allocate instance and initialize some members */
    p_intf->p_sys = malloc( sizeof( intf_sys_t ) );
    if( p_intf->p_sys == NULL )
    {
        intf_ErrMsg("error: %s", strerror(ENOMEM));
        return( 1 );
    }

    /* Initialize Gtk+ thread */
    p_intf->p_sys->b_popup_changed = 0;
    p_intf->p_sys->b_window_changed = 0;
    p_intf->p_sys->b_playlist_changed = 0;

    p_intf->p_sys->b_scale_isfree = 1;

    p_intf->p_sys->pf_gtk_callback = NULL;
    p_intf->p_sys->pf_gdk_callback = NULL;

    /* Initialize lock */
    vlc_mutex_init( &p_intf->p_sys->change_lock );

    return( 0 );
}

/*****************************************************************************
 * intf_Close: destroy interface window
 *****************************************************************************/
static void intf_Close( intf_thread_t *p_intf )
{
    /* Destroy lock */
    vlc_mutex_destroy( &p_intf->p_sys->change_lock );

    /* Destroy structure */
    free( p_intf->p_sys );
}

/*****************************************************************************
 * intf_Run: Gtk+ thread
 *****************************************************************************
 * this part of the interface is in a separate thread so that we can call
 * gtk_main() from within it without annoying the rest of the program.
 * XXX: the approach may look kludgy, and probably is, but I could not find
 * a better way to dynamically load a Gtk+ interface at runtime.
 *****************************************************************************/
static void intf_Run( intf_thread_t *p_intf )
{
    /* gtk_init needs to know the command line. We don't care, so we
     * give it an empty one */
    char *p_args[] = { "" };
    char **pp_args = p_args;
    int i_args = 1;

    /* The data types we are allowed to receive */
    static GtkTargetEntry target_table[] =
    {
        { "text/uri-list", 0, DROP_ACCEPT_TEXT_URI_LIST },
        { "text/plain", 0, DROP_ACCEPT_TEXT_PLAIN }
    };

    /* Initialize Gtk+ */
    gtk_init( &i_args, &pp_args );

    /* Create some useful widgets that will certainly be used */
    p_intf->p_sys->p_window = create_intf_window( );
    p_intf->p_sys->p_popup = create_intf_popup( );

    /* Set the title of the main window */
    gtk_window_set_title( GTK_WINDOW(p_intf->p_sys->p_window),
                          VOUT_TITLE " (Gtk+ interface)");

    /* Accept file drops on the main window */
    gtk_drag_dest_set( GTK_WIDGET( p_intf->p_sys->p_window ),
                       GTK_DEST_DEFAULT_ALL, target_table,
                       1, GDK_ACTION_COPY );

    /* We don't create these ones yet because we perhaps won't need them */
    p_intf->p_sys->p_about = NULL;
    p_intf->p_sys->p_playlist = NULL;
    p_intf->p_sys->p_modules = NULL;
    p_intf->p_sys->p_fileopen = NULL;
    p_intf->p_sys->p_disc = NULL;

    /* Store p_intf to keep an eye on it */
    gtk_object_set_data( GTK_OBJECT(p_intf->p_sys->p_window),
                         "p_intf", p_intf );

    gtk_object_set_data( GTK_OBJECT(p_intf->p_sys->p_popup),
                         "p_intf", p_intf );

    /* Show the control window */
    gtk_widget_show( p_intf->p_sys->p_window );

    /* Sleep to avoid using all CPU - since some interfaces needs to access
     * keyboard events, a 100ms delay is a good compromise */
    p_intf->p_sys->i_timeout = gtk_timeout_add( INTF_IDLE_SLEEP / 1000,
                                                GtkManage, p_intf );
 

    /* Enter Gtk mode */
    gtk_main();

    /* launch stored callbacks */
    if( p_intf->p_sys->pf_gtk_callback != NULL )
    {
        p_intf->p_sys->pf_gtk_callback();

        if( p_intf->p_sys->pf_gdk_callback != NULL )
        {
            p_intf->p_sys->pf_gdk_callback();
        }
    }
}

/* following functions are local */

/*****************************************************************************
 * GtkManage: manage main thread messages
 *****************************************************************************
 * In this function, called approx. 10 times a second, we check what the
 * main program wanted to tell us.
 *****************************************************************************/
static gint GtkManage( gpointer p_data )
{
    intf_thread_t *p_intf = (void *)p_data;

    vlc_mutex_lock( &p_intf->p_sys->change_lock );

    /* If the "display popup" flag has changed */
    if( p_intf->b_menu_change )
    {
        if( !GTK_IS_WIDGET( p_intf->p_sys->p_popup ) )
        {
            p_intf->p_sys->p_popup = create_intf_popup();
            gtk_object_set_data( GTK_OBJECT( p_intf->p_sys->p_popup ),
                                 "p_popup", p_intf );
        }
//        gtk_widget_show( p_intf->p_sys->p_popup );
        gtk_menu_popup( GTK_MENU( p_intf->p_sys->p_popup ),
                        NULL, NULL, NULL, NULL, 0, GDK_CURRENT_TIME );
        p_intf->b_menu_change = 0;
    }

    /* Manage the slider */
    if( p_intf->p_input != NULL && p_intf->p_sys->p_window != NULL
         && p_intf->p_sys->b_scale_isfree )
    {
        GtkWidget *p_scale;
        GtkAdjustment *p_adj;
   
        p_scale = GTK_WIDGET( gtk_object_get_data( GTK_OBJECT(
                                  p_intf->p_sys->p_window ), "hscale" ) );
        p_adj = gtk_range_get_adjustment ( GTK_RANGE( p_scale ) );

        /* Update the value */
        p_adj->value = ( 100. *
                         p_intf->p_input->stream.p_selected_area->i_tell ) /
                         p_intf->p_input->stream.p_selected_area->i_size;

        /* Gtv does it this way. Why not. */
        gtk_range_set_adjustment ( GTK_RANGE( p_scale ), p_adj );
        gtk_range_slider_update ( GTK_RANGE( p_scale ) );
        gtk_range_clear_background ( GTK_RANGE( p_scale ) );
        gtk_range_draw_background ( GTK_RANGE( p_scale ) );
    }

    /* Manage core vlc functions through the callback */
    p_intf->pf_manage( p_intf );

    if( p_intf->b_die )
    {
        /* Make sure we won't be called again */
        gtk_timeout_remove( p_intf->p_sys->i_timeout );

        vlc_mutex_unlock( &p_intf->p_sys->change_lock );

        /* Prepare to die, young Skywalker */
        gtk_main_quit();
        return( FALSE );
    }

    vlc_mutex_unlock( &p_intf->p_sys->change_lock );

    return( TRUE );
}

