import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vitejs.dev/config/
export default defineConfig({
    plugins: [react()],
    // Electron でローカルファイルとして読み込む際に必要
    base: './',
    build: {
        outDir: 'dist',
        assetsDir: 'assets',
    },
})
