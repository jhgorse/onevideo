/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
 *  Author(s): Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ovg-app.h"

#ifdef GDK_WINDOWING_X11
#include <gmodule.h>
#define __USE_GNU
#include <dlfcn.h>
#endif

int
main (int argc, char * argv[])
{
#ifdef GDK_WINDOWING_X11
  GModule *module;
  int (*XInitThreads) (void);

  if (!g_module_supported ())
    goto done;

  module = g_module_open (NULL, 0);
  if (!module)
    goto done;

  /* gstgtkglsink uses threaded Xlib, so we need to initialize threading in Xlib
   * by calling XInitThreads() before any other calls to Xlib
   *
   * If GTK+ is linked with libX11, this will return a pointer to XInitThreads()
   * which we need to call before GTK+ does anything; which means we need to
   * call this as the first thing in main()
   *
   * If GTK+ is built with X11 but is running under a different backend, calling
   * this will not cause problems anyway, so no need to call it conditionally */
  if (!g_module_symbol (module, "XInitThreads", (gpointer *) &XInitThreads))
    goto done;

  if (XInitThreads == NULL)
    goto done;

  if (XInitThreads () == 0) {
    g_printerr ("FATAL: Unable to initialize Xlib threading\n");
    return -1;
  } else {
    g_print ("Xlib threading initialized\n");
  }
done:
#endif

  return g_application_run (G_APPLICATION (ovg_app_new ()), argc, argv);
}
