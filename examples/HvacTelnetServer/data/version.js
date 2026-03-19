window.IR_SERVER_UI_FS_VERSION = "0.2.0";

(function () {
  function injectStyles() {
    if (document.getElementById('ir-version-banner-style')) return;
    const style = document.createElement('style');
    style.id = 'ir-version-banner-style';
    style.textContent = [
      '.ir-version-banner{margin:0 0 16px;padding:12px 14px;border-radius:12px;border:1px solid rgba(239,68,68,.45);background:linear-gradient(180deg,rgba(239,68,68,.12),rgba(239,68,68,.04));color:#fecaca;font-size:.82rem;line-height:1.45}',
      '.ir-version-banner strong{display:block;margin-bottom:4px;color:#fff}',
      '.ir-version-banner code{background:rgba(255,255,255,.08);padding:2px 6px;border-radius:8px;color:#fff}'
    ].join('');
    document.head.appendChild(style);
  }

  function bannerContainer() {
    return document.querySelector('.container') || document.body;
  }

  function ensureBanner() {
    let el = document.getElementById('ir_version_banner');
    if (el) return el;
    el = document.createElement('div');
    el.id = 'ir_version_banner';
    el.className = 'ir-version-banner';
    const container = bannerContainer();
    if (container.firstChild) container.insertBefore(el, container.firstChild);
    else container.appendChild(el);
    return el;
  }

  async function checkVersions() {
    injectStyles();
    try {
      const res = await fetch('/api/status', { cache: 'no-store' });
      if (!res.ok) return;
      const data = await res.json();
      const firmware = data.firmware_version || 'unknown';
      const fsCurrent = data.filesystem_version || 'unknown';
      const fsExpected = data.filesystem_version_expected || 'unknown';
      const uiFs = window.IR_SERVER_UI_FS_VERSION || 'unknown';
      const mismatched = !data.version_match || uiFs !== fsExpected;
      if (!mismatched) return;
      const banner = ensureBanner();
      banner.innerHTML = '<strong>Version mismatch detected</strong>' +
        'Firmware <code>' + firmware + '</code> expects filesystem <code>' + fsExpected + '</code>, ' +
        'but the running UI/filesystem is <code>' + fsCurrent + '</code> (page bundle <code>' + uiFs + '</code>). ' +
        'Update the firmware or SPIFFS image so both are on the same release.';
    } catch (_err) {
      // Ignore version banner failures. The main page should still work.
    }
  }

  document.addEventListener('DOMContentLoaded', checkVersions);
})();
