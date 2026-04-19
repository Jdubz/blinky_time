import { HashRouter, Routes, Route } from 'react-router-dom';
import { DeviceList } from './routes/DeviceList';
import { DeviceDetail } from './routes/DeviceDetail';
import { Fleet } from './routes/Fleet';
import './styles.css';

/**
 * Root component — routes between device list and device detail views.
 *
 * Uses HashRouter (not BrowserRouter) so deep links work both when served
 * from blinky-server (which has SPA fallback) and from Firebase Hosting
 * (which uses firebase.json rewrites). Hash routing avoids needing any
 * server-side configuration.
 *
 * When only one device is discovered, DeviceList auto-navigates to it,
 * preserving the pre-routing single-device UX.
 */
function App() {
  return (
    <HashRouter>
      <Routes>
        <Route path="/" element={<DeviceList />} />
        <Route path="/device/:deviceId" element={<DeviceDetail />} />
        <Route path="/fleet" element={<Fleet />} />
      </Routes>
    </HashRouter>
  );
}

export default App;
