import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { OfflineBanner } from './OfflineBanner';

describe('OfflineBanner', () => {
  it('renders nothing when online', () => {
    const { container } = render(<OfflineBanner isOnline={true} />);
    expect(container.firstChild).toBeNull();
  });

  it('renders the offline banner when offline', () => {
    render(<OfflineBanner isOnline={false} />);
    expect(
      screen.getByText("You're offline. Some features may be unavailable.")
    ).toBeInTheDocument();
  });

  it('displays the warning icon when offline', () => {
    render(<OfflineBanner isOnline={false} />);
    expect(screen.getByText('!')).toBeInTheDocument();
  });

  it('has the correct CSS class when offline', () => {
    render(<OfflineBanner isOnline={false} />);
    const banner = screen
      .getByText("You're offline. Some features may be unavailable.")
      .closest('div');
    expect(banner).toHaveClass('offline-banner');
  });
});
