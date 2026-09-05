# SPDX-License-Identifier: GPL-2.0-or-later
"""Falcon Render: first-run preferences.

Runs once, only when the user has no ``userpref.blend`` yet (a fresh install).
Sets two UI preferences and saves them; after that this file never touches the
preferences again, so everything stays editable in Preferences as usual.

  view.ui_scale = 1.25
      Preferences > Interface > Display > Resolution Scale
  view.filebrowser_display_type = 'SCREEN'
      Preferences > Interface > Temporary Editors > File Browser = Maximized Area

Why: on wlroots compositors (Hyprland / Sway) the file browser window opens at
its minimum size 320x240 (Blender #162315); 'SCREEN' opens it as a maximized
area and avoids creating a new OS window at all.  1.25 is the scale for
~110 PPI monitors viewed at arm's length (factory 1.0 is sized for ~96 PPI).

Skipped in background mode, with --factory-startup, or with FALCON_NO_FIRST_RUN=1.
"""

import os

import bpy


def _apply():
    try:
        if bpy.app.background or bpy.app.factory_startup:
            return None
        if os.environ.get("FALCON_NO_FIRST_RUN") == "1":
            return None
        cfg = bpy.utils.user_resource('CONFIG')
        if not cfg or os.path.exists(os.path.join(cfg, "userpref.blend")):
            return None
        view = bpy.context.preferences.view
        view.ui_scale = 1.25
        view.filebrowser_display_type = 'SCREEN'
        bpy.ops.wm.save_userpref()
        print("Falcon Render: first run - Resolution Scale 1.25, File Browser as Maximized Area "
              "(saved to userpref.blend; change them in Preferences > Interface)")
    except Exception as ex:  # never let a preference nicety break startup
        print("Falcon Render: first-run preferences skipped:", ex)
    return None


def register():
    bpy.app.timers.register(_apply, first_interval=0.0)


def unregister():
    pass
