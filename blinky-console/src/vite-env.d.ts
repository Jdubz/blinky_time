/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** 'on' shows the browser firmware updater (MainShell FIRMWARE_UPDATES_ENABLED). */
  readonly VITE_FIRMWARE_UPDATES?: string;
}
