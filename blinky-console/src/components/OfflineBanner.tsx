interface OfflineBannerProps {
  isOnline: boolean;
}

export function OfflineBanner({ isOnline }: OfflineBannerProps) {
  if (isOnline) return null;

  return (
    <div className="offline-banner">
      <span className="offline-icon">!</span>
      <span>You're offline. Some features may be unavailable.</span>
    </div>
  );
}
