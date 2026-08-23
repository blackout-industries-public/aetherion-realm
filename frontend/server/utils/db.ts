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

// Every endpoint used to swallow SQL errors into an empty array with zero logging -
// which is how a collation error and an ONLY_FULL_GROUP_BY error each shipped invisibly.
// Pages still degrade to empty sections, but each distinct failure now logs once and the
// most recent one is exposed for the Ops tab.
const seenErrors = new Set<string>()
let lastError: { at: number; message: string; sql: string } | null = null

export const dbLastError = () => lastError

export async function q(sql: string, params?: any[]): Promise<any[]> {
  try {
    const [rows] = await getPool().query<any[]>(sql, params)
    return rows as any[]
  } catch (err: any) {
    const message = String(err?.sqlMessage ?? err?.message ?? err)
    lastError = { at: Date.now(), message, sql: sql.trim().replace(/\s+/g, ' ').slice(0, 160) }
    if (!seenErrors.has(message)) {
      seenErrors.add(message)
      console.error(`[db] ${message} :: ${lastError.sql}`)
    }
    return []
  }
}
