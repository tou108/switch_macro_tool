const { app, BrowserWindow, dialog, session } = require('electron');
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
      preload: path.join(__dirname, 'preload.cjs'),
    },
  });

  mainWindow.webContents.session.setPermissionCheckHandler(
    (_wc, permission) => permission === 'bluetooth'
  );
  mainWindow.webContents.session.setPermissionRequestHandler(
    (_wc, permission, callback) => callback(permission === 'bluetooth')
  );

  mainWindow.webContents.on('select-bluetooth-device', (event, deviceList, callback) => {
    event.preventDefault();
    if (deviceList.length === 0) { setTimeout(() => callback(''), 500); return; }
    const buttons = deviceList.map(d => d.deviceName || d.deviceId);
    buttons.push('キャンセル');
    dialog.showMessageBox(mainWindow, {
      type: 'question',
      title: 'Bluetoothデバイスを選択',
      message: '接続するM5Atomを選んでください:',
      buttons,
      cancelId: buttons.length - 1,
    }).then(({ response }) => {
      callback(response >= deviceList.length ? '' : deviceList[response].deviceId);
    });
  });

  mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  mainWindow.on('closed', () => { mainWindow = null; });
}

app.whenReady().then(() => {
  createWindow();
  app.on('activate', () => { if (!mainWindow) createWindow(); });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
