// File System Access API pieces TypeScript's DOM lib doesn't ship
// (Chromium-only: https://wicg.github.io/file-system-access/).
declare global {
  interface DirectoryPickerOptions {
    id?: string;
    mode?: 'read' | 'readwrite';
    startIn?: string;
  }
  interface Window {
    showDirectoryPicker?: (options?: DirectoryPickerOptions) => Promise<FileSystemDirectoryHandle>;
  }
}

export {};
