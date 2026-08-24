import { UiSpark, T } from 'aetherion-observatory'

const HOURS = ['00:00', '03:00', '06:00', '09:00', '12:00', '15:00', '18:00', '21:00', '23:00']
const MAIL = [412, 118, 64, 236, 590, 871, 1240, 2972, 1610]

export function Canonical() {
  return (
    <div style={{ width: 320, background: T.bg, padding: 12 }}>
      <UiSpark
        points={HOURS.map((h, i) => ({ label: h, value: MAIL[i]! }))}
        label="mail runs"
      />
    </div>
  )
}

export function PairedSeries() {
  const kills = [96, 44, 31, 88, 152, 240, 388, 512, 300]
  const deaths = [61, 30, 22, 54, 99, 160, 251, 342, 200]
  return (
    <div style={{ width: 320, background: T.bg, padding: 12 }}>
      <UiSpark
        points={HOURS.map((h, i) => ({ label: h, value: kills[i]!, second: deaths[i]! }))}
        label="honourable kills"
        secondLabel="deaths"
      />
    </div>
  )
}

export function GreenHue() {
  const crafts = [12, 5, 3, 18, 41, 50, 39, 64, 28]
  return (
    <div style={{ width: 320, background: T.bg, padding: 12 }}>
      <UiSpark
        points={HOURS.map((h, i) => ({ label: h, value: crafts[i]! }))}
        hue={T.green}
        label="crafts"
      />
    </div>
  )
}
