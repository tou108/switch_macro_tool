// preload.js
// contextIsolation: true 環境での安全な IPC ブリッジ
// 現状このアプリでは Node 側 API は不要なので最小限のスタブのみ
const { contextBridge } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  platform: process.platform,
});
