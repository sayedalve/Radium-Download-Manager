import os
import sys
import time
import subprocess
import ctypes
from ctypes import wintypes
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

user32.SetProcessDPIAware()

user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.PostMessageW.restype = wintypes.BOOL

kernel32.GlobalAlloc.restype = wintypes.HGLOBAL
kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalUnlock.restype = wintypes.BOOL
kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]

WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wintypes.DWORD),
        ('biWidth', wintypes.LONG),
        ('biHeight', wintypes.LONG),
        ('biPlanes', wintypes.WORD),
        ('biBitCount', wintypes.WORD),
        ('biCompression', wintypes.DWORD),
        ('biSizeImage', wintypes.DWORD),
        ('biXPelsPerMeter', wintypes.LONG),
        ('biYPelsPerMeter', wintypes.LONG),
        ('biClrUsed', wintypes.DWORD),
        ('biClrImportant', wintypes.DWORD)
    ]

class DROPFILES(ctypes.Structure):
    _fields_ = [
        ('pFiles', wintypes.DWORD),
        ('pt', wintypes.POINT),
        ('fNC', wintypes.BOOL),
        ('fWide', wintypes.BOOL)
    ]

def capture_window(hwnd, out_path):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if w <= 0 or h <= 0:
        print(f"[-] Invalid window size {w}x{h} for hwnd {hwnd}")
        return False
        
    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbm = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    old_bm = gdi32.SelectObject(hdc_mem, hbm)
    
    # Try PrintWindow PW_RENDERFULLCONTENT (2)
    res = user32.PrintWindow(hwnd, hdc_mem, 2)
    if not res:
        # Fallback to BitBlt
        gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, rect.left, rect.top, 0x00CC0020)
        
    bmp_header = BITMAPINFOHEADER()
    bmp_header.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmp_header.biWidth = w
    bmp_header.biHeight = -h
    bmp_header.biPlanes = 1
    bmp_header.biBitCount = 32
    bmp_header.biCompression = 0
    
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmp_header), 0)
    
    gdi32.SelectObject(hdc_mem, old_bm)
    gdi32.DeleteObject(hbm)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)
    
    img = Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB')
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    img.save(out_path, quality=95)
    print(f"[+] Successfully saved {out_path} ({w}x{h})")
    return True

def find_window_by_class(class_name, timeout_sec=5.0):
    start = time.time()
    while time.time() - start < timeout_sec:
        found = []
        def enum_cb(hwnd, lparam):
            if user32.IsWindowVisible(hwnd):
                buf = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, buf, 256)
                if buf.value == class_name:
                    found.append(hwnd)
            return True
        user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
        if found:
            return found[0]
        time.sleep(0.1)
    return None

def send_drop_files(hwnd, filepath):
    wpath = os.path.abspath(filepath)
    wbytes = (wpath + '\0\0').encode('utf-16le')
    hdr_size = ctypes.sizeof(DROPFILES)
    total_size = hdr_size + len(wbytes)
    
    GHND = 0x0042 # GMEM_MOVEABLE | GMEM_ZEROINIT
    hGlobal = kernel32.GlobalAlloc(GHND, total_size)
    pGlobal = kernel32.GlobalLock(hGlobal)
    
    df = DROPFILES()
    df.pFiles = hdr_size
    df.pt.x = 100
    df.pt.y = 100
    df.fNC = False
    df.fWide = True
    
    ctypes.memmove(pGlobal, ctypes.byref(df), hdr_size)
    ctypes.memmove(pGlobal + hdr_size, wbytes, len(wbytes))
    kernel32.GlobalUnlock(hGlobal)
    
    WM_DROPFILES = 0x0233
    user32.PostMessageW(hwnd, WM_DROPFILES, wintypes.WPARAM(hGlobal), wintypes.LPARAM(0))

def main():
    exe_path = os.path.abspath("build/win-x64/Release/radium.exe")
    print(f"Launching Radium: {exe_path}")
    proc = subprocess.Popen([exe_path])
    
    try:
        hwnd_main = find_window_by_class("Radium_MainWindowClass", timeout_sec=6.0)
        if not hwnd_main:
            print("[-] Could not find Radium_MainWindowClass window")
            return 1
        print(f"[+] Found main window: {hwnd_main}")
        time.sleep(1.0)
        
        # 1. Capture Settings Dialog Overlay
        print("\n--- 1. Capturing Settings Dialog ---")
        user32.PostMessageW(hwnd_main, 0x0111, 3010, 0) # IDM_CTX_SETTINGS
        time.sleep(1.0)
        capture_window(hwnd_main, "docs/images/readme/radium-settings-dialog.jpg")
        capture_window(hwnd_main, "docs/images/readme/radium-settings-dialog.png")
        
        # Close settings: Escape key
        user32.PostMessageW(hwnd_main, 0x0100, 0x1B, 0) # WM_KEYDOWN VK_ESCAPE
        user32.PostMessageW(hwnd_main, 0x0101, 0x1B, 0) # WM_KEYUP VK_ESCAPE
        time.sleep(0.5)
        
        # 2. Capture New Download Dialog via Add Download Button Click
        print("\n--- 2. Capturing New Download Dialog ---")
        rc = wintypes.RECT()
        user32.GetClientRect(hwnd_main, ctypes.byref(rc))
        w = rc.right - rc.left
        click_x = w - 80
        click_y = 28
        lparam = (click_y << 16) | (click_x & 0xFFFF)
        user32.PostMessageW(hwnd_main, 0x0201, 1, lparam) # WM_LBUTTONDOWN
        user32.PostMessageW(hwnd_main, 0x0202, 0, lparam) # WM_LBUTTONUP
        time.sleep(0.8)
        
        hDlgNew = find_window_by_class("Radium_NewDownloadDlgClass", timeout_sec=4.0)
        if hDlgNew:
            print(f"[+] Found New Download dialog: {hDlgNew}")
            time.sleep(0.5)
            capture_window(hDlgNew, "docs/images/readme/radium-new-download-dialog.jpg")
            capture_window(hDlgNew, "docs/images/readme/radium-new-download-dialog.png")
            # Close dialog: send Escape or close
            user32.PostMessageW(hDlgNew, 0x0010, 0, 0) # WM_CLOSE
            time.sleep(0.5)
        else:
            print("[-] Could not find Radium_NewDownloadDlgClass dialog")
            
        # 3. Capture Add Torrent Dialog
        print("\n--- 3. Capturing Add Torrent Dialog ---")
        torrent_file = os.path.abspath("cyberpunk_edgerunners.torrent")
        if not os.path.exists(torrent_file):
            torrent_file = os.path.abspath("test_nyaa.torrent")
        print(f"Using torrent: {torrent_file}")
        
        send_drop_files(hwnd_main, torrent_file)
        time.sleep(1.0)
        
        hDlgTor = find_window_by_class("Radium_AddTorrentDlgClass", timeout_sec=5.0)
        if hDlgTor:
            print(f"[+] Found Add Torrent dialog: {hDlgTor}")
            time.sleep(0.8)
            capture_window(hDlgTor, "docs/images/readme/radium-add-torrent-dialog.jpg")
            capture_window(hDlgTor, "docs/images/readme/radium-add-torrent-dialog.png")
            # Cancel dialog by posting WM_COMMAND with kAddTorCancelBtn (8223)
            user32.PostMessageW(hDlgTor, 0x0111, 8223, 0)
            time.sleep(0.5)
        else:
            print("[-] Could not find Radium_AddTorrentDlgClass dialog")
            
        print("\n[+] All dialog captures completed!")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            pass

if __name__ == "__main__":
    main()
