import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
    plugins: [react()],
    server: {
        host: '0.0.0.0', // Explicitly bind to IPv4 to prevent Windows IPv6 (::1) conflicts
        port: 5174,      // Shift to a clean port
        watch: {
            usePolling: true, // Required for WSL2 file watching
        }
    }
})