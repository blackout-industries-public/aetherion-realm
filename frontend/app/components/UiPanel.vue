<script setup lang="ts">
import { T, FONT } from '../theme'

defineProps<{
  /** Small caps heading. */
  cap: string
  /** Right-aligned counter or status shown on the same line. */
  note?: string
  /** Removes inner padding for panels that host their own scrolling list. */
  flush?: boolean
  /** Fills the parent and lets the body scroll instead of the page. */
  fill?: boolean
}>()
</script>

<template>
  <section
    class="panel"
    :style="{
      border: `1px solid ${T.line}`,
      background: T.panel,
      boxShadow: T.inset,
      display: 'flex',
      flexDirection: 'column',
      minHeight: 0,
      minWidth: 0,
      ...(fill ? { height: '100%' } : {}),
    }"
  >
    <header
      :style="{
        display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
        gap: '10px', padding: '10px 12px 6px', flex: 'none',
      }"
    >
      <span
        :style="{
          fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
          letterSpacing: '.16em', color: T.dim, textTransform: 'uppercase',
          whiteSpace: 'nowrap',
        }"
      >{{ cap }}</span>
      <span
        v-if="note"
        :style="{
          fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em',
          color: T.faint, whiteSpace: 'nowrap',
        }"
      >{{ note }}</span>
    </header>

    <div
      :style="{
        minHeight: 0, flex: fill ? 1 : 'none',
        overflow: fill ? 'auto' : 'visible',
        padding: flush ? '0' : '0 12px 12px',
      }"
    >
      <slot />
    </div>
  </section>
</template>
