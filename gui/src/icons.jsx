const base = {
  width: 20,
  height: 20,
  viewBox: '0 0 24 24',
  fill: 'none',
  stroke: 'currentColor',
  strokeWidth: 1.8,
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
  'aria-hidden': true,
}

export function OverviewIcon() {
  return <svg {...base}><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/></svg>
}
export function ServerIcon() {
  return <svg {...base}><rect x="3" y="4" width="18" height="6" rx="2"/><rect x="3" y="14" width="18" height="6" rx="2"/><path d="M7 7h.01M7 17h.01M11 7h6M11 17h6"/></svg>
}
export function TunnelIcon() {
  return <svg {...base}><path d="M4 8h11a4 4 0 0 1 0 8H8"/><path d="m7 5-3 3 3 3m2 2-3 3 3 3"/></svg>
}
export function PeerIcon() {
  return <svg {...base}><circle cx="12" cy="5" r="2.5"/><circle cx="5" cy="18" r="2.5"/><circle cx="19" cy="18" r="2.5"/><path d="m10.7 7.2-4.4 8.5m7-8.5 4.4 8.5M7.5 18h9"/></svg>
}
export function DiagnoseIcon() {
  return <svg {...base}><path d="M3 12h4l2-7 4 14 2-7h6"/></svg>
}
export function PlusIcon() {
  return <svg {...base}><circle cx="12" cy="12" r="9"/><path d="M12 8v8m-4-4h8"/></svg>
}
export function CloseIcon() {
  return <svg {...base}><path d="m6 6 12 12M18 6 6 18"/></svg>
}
export function RefreshIcon() {
  return <svg {...base}><path d="M20 6v5h-5M4 18v-5h5"/><path d="M18.3 9A7 7 0 0 0 6.2 6.2L4 8m16 8-2.2 1.8A7 7 0 0 1 5.7 15"/></svg>
}
