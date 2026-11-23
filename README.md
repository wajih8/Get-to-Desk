# Remote Desktop Client

A C++20 and Qt 6 desktop prototype for attended pairing. The Host tab previews a selected monitor locally, creates expiring invitation codes, and gives the host explicit Allow view, Decline and End session controls. The Viewer tab requests access and shows the result and session details.

The client speaks the v1 WebSocket messages in `PROTOCOL.md` as written. Both roles are in one executable, and each instance opens its own connection. The preview uses Qt screen snapshots on the host PC. WebRTC media and remote input are later milestones in the project plan.

## 1. Install the Windows build tools

1. In **Visual Studio Build Tools 2022**, install **Desktop development with C++**. Include **MSVC v143 x64/x86**, a **Windows SDK**, and **C++ CMake tools for Windows**. [Microsoft build tools components](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=visualstudio)
2. In the **Qt Online Installer** or **Qt Maintenance Tool**, select the **MSVC 2022 64-bit** kit and **Qt WebSockets** for the same Qt version. Use Qt 6.8 or later. The commands below use Qt 6.11.3 installed under `C:\Qt`. [Qt installer instructions](https://doc.qt.io/qt-6/qt-online-installation.html)

## 2. Start the Python signaling server

If you already set up `Remote_Desktop_Signaling.zip`, open PowerShell in the extracted server folder and run:

```powershell
.\.venv\Scripts\python.exe .\server.py
```

For a first-time setup, save the server ZIP to Downloads and run these commands with Python 3.12 available through `py`.

```powershell
Set-Location "$env:USERPROFILE\Downloads"
Expand-Archive -LiteralPath .\Remote_Desktop_Signaling.zip -DestinationPath .\Remote_Desktop_Signaling
Set-Location .\Remote_Desktop_Signaling
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\requirements.txt
.\.venv\Scripts\python.exe .\server.py
```

Leave the server window open. Its default address is `ws://127.0.0.1:8765/ws`. Run both test clients on that Windows PC because the development server listens on loopback.

## 3. Build and launch the client

Save `Remote_Desktop_Client.zip` to Downloads. Open **Developer PowerShell for VS 2022** from the Start menu. Set `$qt` to your installed MSVC Qt kit folder if it differs from the example.

```powershell
Set-Location "$env:USERPROFILE\Downloads"
Expand-Archive -LiteralPath .\Remote_Desktop_Client.zip -DestinationPath .\Remote_Desktop_Client
Set-Location .\Remote_Desktop_Client
$qt = 'C:\Qt\6.11.3\msvc2022_64'
$env:Path = "$qt\bin;$env:Path"
cmake -S . -B .\build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$qt"
cmake --build .\build --config Release --parallel
$app = (Resolve-Path .\build\Release\RemoteDesktopClient.exe).Path
& "$qt\bin\windeployqt.exe" --release "$app"
Start-Process -FilePath $app
Start-Process -FilePath $app
```

`windeployqt` places the Qt runtime and plugins beside the release executable. Each window connects automatically when it opens. [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)

## 4. Set your server URL

After building, open the config beside the executable.

```powershell
notepad .\build\Release\config.json
```

Paste the full WebSocket endpoint into `server_url`, keeping the JSON quotes. Use `ws://` or `wss://` and include the server's WebSocket path.

```json
{
  "server_url": "ws://127.0.0.1:8765/ws"
}
```

Save the file. In each open client, click **Disconnect** if connected and then **Connect** to read the new URL. The same file can be edited next to the exe wherever you launch it. For a custom default on a fresh build, edit the source `config.json` before building.

## 5. Check the flow in two windows

1. In the first window's **Host** tab, select a display and click **Start preview**. Move a window on the selected monitor to check that the image updates. If the app is on that monitor, it also appears in the preview.
2. Click **Create invitation** and **Copy code**. The preview stops and the code appears with a countdown.
3. In the second window, open **Viewer**, enter a name, paste the code and click **Request access**. The viewer shows that approval is pending. The host shows the requesting name, the viewer's connection ID and the selected display.
4. In Host, click **Allow view**. Both windows show the same session ID. Click **End session** in either window and check that both return to idle.
5. Create a new invitation and repeat with **Decline**. Create another with **15 seconds** and leave it pending until it expires. Both windows return to idle and the viewer can ask for a fresh code.

To withdraw an open invitation or a pending request, click **Disconnect**. The server invalidates the invitation or closes the request as part of that protocol flow. A fresh connection is available from **Connect**.
