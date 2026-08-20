export default defineNuxtConfig({
  compatibilityDate: '2026-08-19',
  ssr: true,
  devtools: { enabled: false },
  // Read-only view of the realm database. No auth by design: this is a LAN dashboard.
  runtimeConfig: {
    dbHost: process.env.DB_HOST || 'ac-database',
    dbPort: process.env.DB_PORT || '3306',
    dbUser: process.env.DB_USER || 'root',
    dbPassword: process.env.DB_PASSWORD || '',
  },
})
