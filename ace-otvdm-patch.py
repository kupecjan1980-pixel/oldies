#!/usr/bin/env python3
"""Ace Ventura under OTVDM (winevdm): a fix for its crash a few seconds in.

The game posts itself WM_TIMER messages whose lParam is a time stamp. OTVDM's DispatchMessage16 takes any
non-zero lParam of a WM_TIMER for a timer procedure's address and calls it ("Interrupt 0D #GP (FFFF:...)").
This changes dll/user.exe16 so that only OTVDM's own timer thunks (and its TIMER32 marker) are called; any
other WM_TIMER goes to the window's procedure as an ordinary message, as Windows 3.1 did.

Only for OTVDM's development build 2712 (ci.appveyor.com/project/otya128/winevdm): the bytes are checked
first and nothing is written if they differ. The original is kept as user.exe16.orig.

    ace-otvdm-patch.py FOLDER_WITH_OTVDM/dll/user.exe16
"""
import os
import shutil
import sys

TEXT_VA, TEXT_FILE = 0x10001000, 0x400
EXPECT = [(0x10017b5a, '0fb75e048bc781ff60741342752c'),
          (0x10017b94, '3d60741342742e250000ffff3d0000ffff7522ff15e0b10210'),
          (0x10017bc9, '0fb70650')]
PATCH = [(0x10017b5a, 'e94800000090'),                    # to the check at 7ba7
         (0x10017b94, 'eb33'),                            # a real timer: on to DispatchMessageA
         (0x10017b96, '0fb75e048bc7e9bfffffff'),          # timer_ok: wParam, back to 7b60
         (0x10017ba1, '909090909090'),
         (0x10017ba7, '8b0d40a60610'),                    # mov ecx,[timer_thunk_array] (on the old relocation)
         (0x10017bad, '3bf9720a81c1f00f00003bf972db'),    # inside the thunks' array?
         (0x10017bbb, '81ff6074134274d3e945000000'),      # or TIMER32_LPARAM; else an ordinary message
         (0x10017bc8, '90')]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    src = path + '.orig' if os.path.exists(path + '.orig') else path
    data = bytearray(open(src, 'rb').read())
    for va, hexs in EXPECT:
        b = bytes.fromhex(hexs)
        o = va - TEXT_VA + TEXT_FILE
        if data[o:o + len(b)] != b:
            sys.exit('%s is not the build this patch knows (at %x): nothing changed' % (src, va))
    if src == path:
        shutil.copy(path, path + '.orig')
    for va, hexs in PATCH:
        b = bytes.fromhex(hexs)
        o = va - TEXT_VA + TEXT_FILE
        data[o:o + len(b)] = b
    open(path, 'wb').write(data)
    print('patched', path)


main()
