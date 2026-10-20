import { useEffect } from 'react';

/**
 * Keep the screen awake while `active` (a recording, a firmware update):
 * Chrome may throttle or suspend a hidden page. Re-acquired when the page
 * becomes visible again, released when inactive.
 */
export function useWakeLock(active: boolean): void {
  useEffect(() => {
    if (!active || !('wakeLock' in navigator)) return;
    let lock: WakeLockSentinel | null = null;
    let cancelled = false;
    const acquire = async () => {
      try {
        lock = await navigator.wakeLock.request('screen');
      } catch {
        // Denied (e.g. battery saver). The page keeps working while visible.
      }
    };
    const onVisible = () => {
      if (document.visibilityState === 'visible' && !cancelled) void acquire();
    };
    void acquire();
    document.addEventListener('visibilitychange', onVisible);
    return () => {
      cancelled = true;
      document.removeEventListener('visibilitychange', onVisible);
      void lock?.release();
    };
  }, [active]);
}
