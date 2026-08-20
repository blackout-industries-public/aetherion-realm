FROM node:22-slim AS build
WORKDIR /app
RUN corepack enable

# pnpm 11 fails any invocation while esbuild has an unrun postinstall. Verified the
# binary works regardless - pnpm installs the platform-specific @esbuild/* optional
# dependency, which is what actually provides it. Set as an env var rather than a
# flag because `pnpm build` performs the same check, not just `pnpm install`.
# (`--allow-build` does not exist in pnpm 11, and onlyBuiltDependencies in
# pnpm-workspace.yaml did not suppress it.)
ENV PNPM_CONFIG_STRICT_DEP_BUILDS=false

# Dependencies resolve in their own layer so source edits do not re-resolve them.
#
# strictDepBuilds=false: pnpm 11 fails the install because esbuild has a postinstall
# it will not run unattended. Verified that the binary still works without it - pnpm
# installs the platform-specific @esbuild/* optional dependency, which is what
# actually provides it. Neither `--allow-build` (does not exist in pnpm 11) nor
# `onlyBuiltDependencies` in pnpm-workspace.yaml suppressed the error.
COPY package.json ./
RUN pnpm install --no-frozen-lockfile

COPY nuxt.config.ts ./
COPY app ./app
COPY server ./server
RUN pnpm build

FROM node:22-slim AS runtime
WORKDIR /app
ENV NODE_ENV=production NITRO_PORT=3000 NITRO_HOST=0.0.0.0
COPY --from=build /app/.output ./.output
RUN useradd --create-home --uid 1001 web && chown -R web:web /app
USER web
EXPOSE 3000
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD node -e "fetch('http://127.0.0.1:3000/api/world').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
CMD ["node", ".output/server/index.mjs"]
