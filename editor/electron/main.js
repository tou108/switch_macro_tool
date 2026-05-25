const { app, BrowserWindow, dialog, ipcMain, session } = require('electron');
const path = require('path');

let mainWindow;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1280,
    height: 900,
    minWidth: 960,
    minHeight: 600,
    title: 'Switch Macro Tool',
    backgroundColor: '#1a1a1a',
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js'),
    },
  });

  // ── Web Bluetooth 権限を許可 ──────────────────────────────────
  mainWindow.webContents.session.setPermissionCheckHandler(
    (_webContents, permission) => permission === 'bluetooth'
  );
  mainWindow.webContents.session.setPermissionRequestHandler(
    (_webContents, permission, callback) => callback(permission === 'bluetooth')
  );

  // ── Bluetooth デバイス選択ダイアログ ──────────────────────────
  // requestDevice() を呼ぶとこのイベントが発火する
  mainWindow.webContents.on('select-bluetooth-device', (event, deviceList, callback) => {
    event.preventDefault(); // デフォルト動作を抑制

    if (deviceList.length === 0) {
      // デバイスが見つからない場合は少し待ってから空文字でキャンセル
      setTimeout(() => callback(''), 500);
      return;
    }

    // Electron のダイアログでデバイス一覧を表示
    const buttons = deviceList.map(d => d.deviceName || `(${d.deviceId.substring(0, 8)}...)`);
    buttons.push('キャンセル');

    dialog.showMessageBox(mainWindow, {
      type: 'question',
      title: 'Bluetooth デバイスを選択',
      message: '接続する ATOMS3 デバイスを選んでください:',
      buttons,
      cancelId: buttons.length - 1,
    }).then(({ response }) => {
      if (response >= deviceList.length) {
        callback(''); // キャンセル
      } else {
        callback(deviceList[response].deviceId);
      }
    });
  });

  // ── アプリ読み込み ────────────────────────────────────────────
  const isDev = process.env.NODE_ENV === 'development';
  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools();
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }

  mainWindow.on('closed', () => { mainWindow = null; });
}

app.whenReady().then(() => {
  createWindow();
  app.on('activate', () => { if (!mainWindow) createWindow(); });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
