import mysql from 'mysql2/promise'

let pool: mysql.Pool | null = null

// One pool for the process. The dashboard polls every few seconds, so opening a
// connection per request would be the dominant cost.
export function getPool() {
  if (pool) return pool
  const cfg = useRuntimeConfig()
  pool = mysql.createPool({
    host: cfg.dbHost,
    port: Number(cfg.dbPort),
    user: cfg.dbUser,
    password: cfg.dbPassword,
    // No default schema: queries span acore_characters and acore_playerbots.
    connectionLimit: 4,
    waitForConnections: true,
  })
  return pool
}
