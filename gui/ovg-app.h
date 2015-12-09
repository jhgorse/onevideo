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


#ifndef __OVG_APP_H__
#define __OVG_APP_H__

#include "onevideo/lib.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define OVG_TYPE_APP         (ovg_app_get_type ())
#define OVG_APP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OVG_TYPE_APP, OvgApp))
#define OVG_APP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), OVG_TYPE_APP, OvgAppClass))
#define OVG_IS_APP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OVG_TYPE_APP))
#define OVG_IS_APP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OVG_TYPE_APP))
#define OVG_APP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OVG_TYPE_APP, OvgAppClass))

typedef struct _OvgApp        OvgApp;
typedef struct _OvgAppClass   OvgAppClass;
typedef struct _OvgAppPrivate OvgAppPrivate;

GType               ovg_app_get_type          (void) G_GNUC_CONST;
OvgApp*             ovg_app_new               (void);
OneVideoLocalPeer*  ovg_app_get_ov_local_peer (OvgApp *app);

G_END_DECLS

#endif /* __OVG_APP_H__ */
