import { Component, ErrorInfo, ReactNode } from 'react';

interface Props {
  children: ReactNode;
  fallback?: ReactNode;
  onError?: (error: Error, errorInfo: ErrorInfo) => void;
}

interface State {
  hasError: boolean;
  error: Error | null;
  errorInfo: ErrorInfo | null;
}

/**
 * ErrorBoundary - Catches React component errors and prevents app-wide crashes
 *
 * Features:
 * - Catches errors in child components during rendering, lifecycle methods, and constructors
 * - Displays a user-friendly error message with details
 * - Provides a "Reload" button to recover
 * - Logs errors to console for debugging
 * - Optional error reporting callback for analytics/telemetry integration
 *
 * Note: Error boundaries do NOT catch:
 * - Event handlers (use try-catch)
 * - Asynchronous code (setTimeout, promises)
 * - Server-side rendering
 * - Errors in the error boundary itself
 */
export class ErrorBoundary extends Component<Props, State> {
  constructor(props: Props) {
    super(props);
    this.state = {
      hasError: false,
      error: null,
      errorInfo: null,
    };
  }

  static getDerivedStateFromError(error: Error): Partial<State> {
    // Update state so the next render will show the fallback UI
    return { hasError: true, error };
  }

  componentDidCatch(error: Error, errorInfo: ErrorInfo): void {
    // Log error details to console
    console.error('ErrorBoundary caught an error:', error);
    console.error('Component stack:', errorInfo.componentStack);

    // Update state with error details
    this.setState({
      error,
      errorInfo,
    });

    // Call optional error reporting callback for analytics/telemetry
    if (this.props.onError) {
      try {
        this.props.onError(error, errorInfo);
      } catch (reportingError) {
        // Don't let error reporting itself crash the error boundary
        console.error('Error in error reporting callback:', reportingError);
      }
    }
  }

  handleReload = (): void => {
    // Reset error state and reload the page
    window.location.reload();
  };

  handleReset = (): void => {
    // Reset error state without reloading (try to recover)
    this.setState({
      hasError: false,
      error: null,
      errorInfo: null,
    });
  };

  render(): ReactNode {
    if (this.state.hasError) {
      // Use custom fallback if provided
      if (this.props.fallback) {
        return this.props.fallback;
      }

      // Default error UI
      return (
        <div className="error-boundary">
          <div className="error-boundary-content">
            <div className="error-boundary-icon">⚠️</div>
            <h1>Something Went Wrong</h1>
            <p className="error-boundary-message">
              The application encountered an unexpected error and couldn't continue.
            </p>

            {this.state.error && (
              <details className="error-boundary-details">
                <summary>Error Details</summary>
                <div className="error-boundary-stack">
                  <strong>Error:</strong> {this.state.error.toString()}
                  {this.state.errorInfo && (
                    <>
                      <br />
                      <br />
                      <strong>Component Stack:</strong>
                      <pre>{this.state.errorInfo.componentStack}</pre>
                    </>
                  )}
                </div>
              </details>
            )}

            <div className="error-boundary-actions">
              <button className="btn btn-primary" onClick={this.handleReload}>
                Reload Application
              </button>
              <button className="btn btn-secondary" onClick={this.handleReset}>
                Try to Continue
              </button>
            </div>

            <p className="error-boundary-hint">
              If this problem persists, try disconnecting and reconnecting your device.
            </p>
          </div>
        </div>
      );
    }

    return this.props.children;
  }
}
