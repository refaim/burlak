Burlak for Far Manager 3
************************

Drag files out of Far straight into other programs: drop them into Telegram,
into a browser's upload box, into Explorer, into a chat.

How to use:
  Press the left mouse button on a file, move the mouse a little and drop
  the file where you want it. Clicks work as usual.
  Drop onto Far's other panel to copy there; hold Shift to move instead,
  including to or from plugin panels.
  Burlak also accepts drops from Explorer, other programs and another Far onto
  panels backed by real directories; it copies by default and moves with Shift,
  with the shell's progress shown in the receiving Far. Both Fars need Burlak
  for a Far-to-Far drop. If several Fars share one Windows Terminal window, the
  drop goes to the tab that most recently had focus.
  Files and selections can also be dragged out of archive, FTP/SFTP, and
  other plugin panels; after release they are extracted as copies for the target,
  so these plugin-panel drags do not offer shortcuts.
  Files dragged out of a plugin panel are extracted into %TEMP%\Burlak and
  removed a few minutes later, giving the receiving program time to read
  them.
  A drop into a plugin panel such as an archive or FTP is not supported yet
  and the terminal pastes the path as before; Windows also blocks a drop from
  a non-elevated program into elevated Far.

  Drag with the right button if you want the target to ask what to do:
  Explorer will offer to copy, move or make a shortcut. Telegram and the
  like don't ask, they just take the files. A right click still selects a
  file, but sweeping over files with the right button held now drags them
  instead of selecting them.

  Works in the plain console, in OpenConsole and in Windows Terminal.

Install:
  Unpack the archive into Far's Plugins folder and restart Far.

Licence: MIT. Far's plugin headers are used under their own BSD-3-clause
licence, (c) 1996 Eugene Roshal, (c) 2000 Far Group.

Roman Kharitonov
  https://github.com/refaim/burlak
  https://plugring.farmanager.com/plugin.php?pid=989
