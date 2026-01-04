/**
 * Toast notification utilities for blinky-console
 *
 * Wraps react-hot-toast with consistent styling and helper functions.
 *
 * Usage:
 *   import { notify } from './lib/toast';
 *   notify.success('Settings saved');
 *   notify.error('Connection failed');
 *   notify.promise(asyncOperation(), {
 *     loading: 'Saving...',
 *     success: 'Saved!',
 *     error: 'Failed to save'
 *   });
 */

import toast from 'react-hot-toast';

// Default durations
const DURATION = {
  success: 2000,
  error: 4000,
  info: 3000,
};

export const notify = {
  /**
   * Show a success notification
   */
  success: (message: string) => {
    return toast.success(message, {
      duration: DURATION.success,
      style: {
        background: '#10b981',
        color: '#fff',
      },
      iconTheme: {
        primary: '#fff',
        secondary: '#10b981',
      },
    });
  },

  /**
   * Show an error notification
   */
  error: (message: string) => {
    return toast.error(message, {
      duration: DURATION.error,
      style: {
        background: '#ef4444',
        color: '#fff',
      },
      iconTheme: {
        primary: '#fff',
        secondary: '#ef4444',
      },
    });
  },

  /**
   * Show an info notification
   */
  info: (message: string) => {
    return toast(message, {
      duration: DURATION.info,
      icon: 'ℹ️',
      style: {
        background: '#3b82f6',
        color: '#fff',
      },
    });
  },

  /**
   * Show a loading notification (must be dismissed manually)
   */
  loading: (message: string) => {
    return toast.loading(message, {
      style: {
        background: '#6b7280',
        color: '#fff',
      },
    });
  },

  /**
   * Dismiss a specific toast or all toasts
   */
  dismiss: (id?: string) => {
    if (id) {
      toast.dismiss(id);
    } else {
      toast.dismiss();
    }
  },

  /**
   * Handle an async operation with loading/success/error states
   */
  promise: <T>(
    promise: Promise<T>,
    messages: {
      loading: string;
      success: string;
      error: string | ((err: Error) => string);
    }
  ) => {
    return toast.promise(promise, messages, {
      style: {
        minWidth: '200px',
      },
      success: {
        duration: DURATION.success,
        style: {
          background: '#10b981',
          color: '#fff',
        },
      },
      error: {
        duration: DURATION.error,
        style: {
          background: '#ef4444',
          color: '#fff',
        },
      },
    });
  },

  /**
   * Update an existing toast
   */
  update: (id: string, message: string, type: 'success' | 'error' | 'loading') => {
    if (type === 'success') {
      toast.success(message, { id });
    } else if (type === 'error') {
      toast.error(message, { id });
    } else {
      toast.loading(message, { id });
    }
  },
};

// Re-export toast for advanced usage
export { toast };
