# Remote Desktop Client Setup

Windows x64 guide for the C++ and Qt client. The same executable has **Host** and **Viewer** tabs. This prototype supports attended invitations and a host-only screen preview.

## 1. Install the build tools

Recheck these entries in Visual Studio Installer.

| Where | Select |
|---|---|
| Workloads | **Desktop development with C++** |
| Optional | **MSVC v143 – VS 2022 C++ x64/x86 build tools** |
| Optional | **C++ CMake tools for Windows** |
| Optional | **Windows 11 SDK (10.0.26100.9457)**, or the available 10.0.26100 SDK |

**C++ core desktop features** is included automatically. Other optional components are unnecessary for this project. [Microsoft Build Tools component list](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2026)

In the **Qt Online Installer** or **Qt Maintenance Tool**, install **Qt 6.11.3 → MSVC 2022 64-bit** and **Qt WebSockets** for that Qt version. [Qt installation guide](https://doc.qt.io/qt-6/qt-online-installation.html)

## 2. Extract and build

Save the existing client ZIP in Downloads. Open **Developer PowerShell** for your installed Visual Studio version and run the following. If you already extracted the ZIP, skip the extraction line. Adjust the Qt path if needed.

~~~powershell
Set-Location "$env:USERPROFILE\Downloads"
Expand-Archive -LiteralPath .\Remote_Desktop_Client.zip -DestinationPath .\Remote_Desktop_Client
Set-Location .\Remote_Desktop_Client
$qt = 'C:\Qt\6.11.3\msvc2022_64'
$env:Path = "$qt\bin;$env:Path"
cmake --version
~~~

Run one configure command for your installed Visual Studio version. The Visual Studio 2026 generator requires CMake 4.2 or later. [CMake Visual Studio 2026 generator documentation](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)

**Visual Studio Build Tools 2026**

~~~powershell
cmake -S . -B .\build-vs2026 -G "Visual Studio 18 2026" -A x64 -T v143 "-DCMAKE_PREFIX_PATH=$qt"
$build = '.\build-vs2026'
~~~

**Visual Studio Build Tools 2022**

~~~powershell
cmake -S . -B .\build-vs2022 -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$qt"
$build = '.\build-vs2022'
~~~

Once configuration succeeds, build and deploy the Qt runtime files. [Qt Windows deployment guide](https://doc.qt.io/qt-6/windows-deployment.html)

~~~powershell
cmake --build $build --config Release --parallel
$app = (Resolve-Path (Join-Path $build 'Release\RemoteDesktopClient.exe')).Path
& "$qt\bin\windeployqt.exe" --release "$app"
~~~

## 3. Configure and launch the client

Edit the config file beside the executable.

~~~powershell
notepad (Join-Path $build 'Release\config.json')
~~~

Set the full WebSocket endpoint, including its path. The default is for a service running on the same PC.

~~~json
{
  "server_url": "ws://127.0.0.1:8765/ws"
}
~~~

With the configured endpoint running, open two client windows. If you edit the URL while the clients are open, click **Disconnect** and then **Connect** in each window.

~~~powershell
Start-Process -FilePath $app
Start-Process -FilePath $app
~~~

## 4. Test the two client windows

1. In **Host**, select a display and click **Start preview** to check the local image.
2. Click **Create invitation**, then **Copy code**.
3. In the second window, open **Viewer**, enter a name, paste the code, and click **Request access**.
4. In **Host**, click **Allow view**. Confirm that both windows show the same session ID, then click **End session** in either window.
5. Use fresh codes to test **Decline** and invitation expiry.

The Viewer displays session status and permissions. Screen streaming and remote input are outside this prototype.
