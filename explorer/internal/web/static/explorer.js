// Refresh live pages in place every 10 s; the chart only animates on first load.
(() => {
  // Age the actual last check even while polling is paused or unavailable.
  function updateCheckAge() {
    const label = document.querySelector('#ledger-status time[datetime]');
    if (!label) return;
    const checked = Date.parse(label.dateTime);
    if (!Number.isFinite(checked)) return;
    const seconds = Math.max(0, Math.floor((Date.now() - checked) / 1000));
    label.textContent = seconds < 60 ? `${seconds}s ago`
      : seconds < 3600 ? `${Math.floor(seconds / 60)}m ago`
      : seconds < 172800 ? `${Math.floor(seconds / 3600)}h ago`
      : `${Math.floor(seconds / 86400)}d ago`;
  }
  updateCheckAge();
  setInterval(updateCheckAge, 1000);
  if (!document.querySelector('[data-live]')) return;
  document.documentElement.classList.add('armed');
  let refreshing = false;
  setInterval(async () => {
    if (refreshing || document.hidden || document.activeElement?.name === 'q') return;
    refreshing = true;
    try {
      const r = await fetch(location.href, { headers: { 'X-Partial': '2' }, cache: 'no-store' });
      if (!r.ok) return;
      const fresh = new DOMParser().parseFromString(await r.text(), 'text/html');
      const ids = ['live', 'ledger-status', 'peer-status'];
      if (ids.some(id => !fresh.getElementById(id))) return;
      document.documentElement.classList.add('settled');
      for (const id of ids) {
        document.getElementById(id).innerHTML = fresh.getElementById(id).innerHTML;
      }
      updateCheckAge();
    } catch {} finally { refreshing = false; }
  }, 10000);
})();
