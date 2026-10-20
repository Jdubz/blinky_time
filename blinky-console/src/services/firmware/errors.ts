/**
 * Every refusal in the web flasher is a FirmwareError: the UI shows its
 * message verbatim and stops. Nothing in services/firmware substitutes a
 * default or "best effort" value for a failed check (CLAUDE.md: no silent
 * fallbacks).
 */
export class FirmwareError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'FirmwareError';
  }
}

export function hex32(n: number): string {
  return '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
}
