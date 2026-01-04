import React from 'react';
import ReactDOM from 'react-dom/client';
import { Toaster } from 'react-hot-toast';
import App from './App';
import { ErrorBoundary } from './components/ErrorBoundary';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ErrorBoundary>
      <App />
      <Toaster
        position="bottom-right"
        toastOptions={{
          // Default options for all toasts
          style: {
            borderRadius: '8px',
            padding: '12px 16px',
          },
        }}
      />
    </ErrorBoundary>
  </React.StrictMode>
);
