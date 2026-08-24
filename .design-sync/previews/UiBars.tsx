import { UiBars, STATE, T } from 'aetherion-observatory'

export function Ranked() {
  const rows = [
    { label: 'mailbox', value: 694 },
    { label: 'gather', value: 587 },
    { label: 'auction house', value: 182 },
    { label: 'trainer', value: 91 },
    { label: 'crafting focus', value: 46 },
    { label: 'vendor', value: 5 },
  ]
  return (
    <div style={{ width: 340, background: T.bg, padding: 12 }}>
      <UiBars rows={rows} labelWidth="128px" />
    </div>
  )
}

export function TonedStates() {
  const rows = [
    { label: 'questing', value: 811, tone: STATE.questing.color },
    { label: 'travelling', value: 402, tone: STATE.travelling.color },
    { label: 'fighting', value: 366, tone: STATE.grinding.color },
    { label: 'in town', value: 254, tone: STATE.town.color },
    { label: 'idle', value: 190, tone: STATE.idle.color },
  ]
  return (
    <div style={{ width: 340, background: T.bg, padding: 12 }}>
      <UiBars rows={rows} />
    </div>
  )
}

export function CappedWithNotes() {
  const rows = [
    { label: 'repairs', value: 2029, note: 'funded' },
    { label: 'training', value: 342, note: 'funded' },
    { label: 'better gear', value: 245, note: 'of 7,749' },
  ]
  return (
    <div style={{ width: 340, background: T.bg, padding: 12 }}>
      <UiBars rows={rows} max={7749} unit="" labelWidth="128px" />
    </div>
  )
}
