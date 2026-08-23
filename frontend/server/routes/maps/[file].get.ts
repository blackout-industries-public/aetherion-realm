import { createReadStream, existsSync, statSync } from 'node:fs'
import { join } from 'node:path'

// Served at runtime rather than as a Nitro public asset: Nitro fixes its public
// manifest at build time, so a bind-mounted file added later is invisible to it and
// the request falls through to the HTML handler. Reading from disk keeps the
// "drop a file in maps/ and refresh" behaviour working.
const MAP_DIR = process.env.MAP_DIR || '/app/maps'

// Continent ids plus generated zone files. A strict pattern rather than sanitising,
// which removes any path-traversal question - nothing outside these two shapes is
// ever read from disk.
const ALLOWED = /^(?:0|1|530|571|zone-\d{1,5})\.jpg$/

export default defineEventHandler(event => {
  const file = getRouterParam(event, 'file') ?? ''
  if (!ALLOWED.test(file)) throw createError({ statusCode: 404, statusMessage: 'Not found' })

  const path = join(MAP_DIR, file)
  if (!existsSync(path)) throw createError({ statusCode: 404, statusMessage: 'No art for that map' })

  setHeader(event, 'Content-Type', 'image/jpeg')
  setHeader(event, 'Content-Length', statSync(path).size)
  // Short cache: art changes rarely, but a stale image after replacing one is worse
  // than re-fetching a couple of hundred KB.
  setHeader(event, 'Cache-Control', 'public, max-age=60')
  return sendStream(event, createReadStream(path))
})
