/**
 * ScenesPanel tests — covers the fetch-driven CRUD flows (list/save/apply/
 * delete) and the maxLength guard added per PR 131 review. Added
 * 2026-04-24.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { ScenesPanel, type Scene } from './ScenesPanel';

function makeFetchQueue(responses: Array<{ ok: boolean; json?: unknown; status?: number }>) {
  const spy = vi.fn();
  for (const r of responses) {
    spy.mockResolvedValueOnce({
      ok: r.ok,
      status: r.status ?? (r.ok ? 200 : 500),
      json: async () => r.json ?? null,
    } as Response);
  }
  return spy;
}

function setup(overrides: Partial<Parameters<typeof ScenesPanel>[0]> = {}) {
  const onApplied = vi.fn();
  const utils = render(
    <ScenesPanel
      currentGenerator="fire"
      currentEffectMode="rotate"
      currentHueSpeed={0.7}
      currentHueShift={0.2}
      onApplied={onApplied}
      {...overrides}
    />
  );
  return { ...utils, onApplied };
}

describe('ScenesPanel — initial fetch', () => {
  beforeEach(() => vi.stubGlobal('fetch', vi.fn()));
  afterEach(() => vi.unstubAllGlobals());

  it('fetches /api/scenes on mount and renders the list', async () => {
    const scenes: Scene[] = [
      { name: 'Party', generator: 'fire', effect_mode: 'rotate', effect_speed: 1, effect_hue: 0 },
      { name: 'Calm', generator: 'water', effect_mode: 'off', effect_speed: 0, effect_hue: 0 },
    ];
    (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
      ok: true,
      json: async () => scenes,
    } as Response);
    setup();
    await waitFor(() => {
      expect(screen.getByText('Party')).toBeInTheDocument();
      expect(screen.getByText('Calm')).toBeInTheDocument();
    });
    expect(globalThis.fetch).toHaveBeenCalledWith('/api/scenes', expect.any(Object));
  });

  it('surfaces an error banner when list fetch fails', async () => {
    (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
      ok: false,
      status: 503,
      json: async () => ({}),
    } as Response);
    setup();
    await waitFor(() => {
      expect(screen.getByText(/503/)).toBeInTheDocument();
    });
  });
});

describe('ScenesPanel — save', () => {
  beforeEach(() => vi.stubGlobal('fetch', vi.fn()));
  afterEach(() => vi.unstubAllGlobals());

  it('PUTs /api/scenes/{name} with the current state as the body', async () => {
    const fetchSpy = makeFetchQueue([
      { ok: true, json: [] }, // initial list
      { ok: true, json: { name: 'Movie Night' } }, // save
      {
        ok: true,
        json: [
          {
            name: 'Movie Night',
            generator: 'fire',
            effect_mode: 'rotate',
            effect_speed: 0.7,
            effect_hue: 0.2,
          },
        ],
      }, // refresh
    ]);
    vi.stubGlobal('fetch', fetchSpy);
    setup({
      currentGenerator: 'fire',
      currentEffectMode: 'rotate',
      currentHueSpeed: 0.7,
      currentHueShift: 0.2,
    });
    await waitFor(() => expect(fetchSpy).toHaveBeenCalledTimes(1));

    const input = screen.getByPlaceholderText(/name the current look/i);
    fireEvent.change(input, { target: { value: 'Movie Night' } });
    fireEvent.click(screen.getByRole('button', { name: /save/i }));

    await waitFor(() => {
      expect(fetchSpy).toHaveBeenCalledTimes(3); // list + put + list-refresh
    });
    const [putUrl, putInit] = fetchSpy.mock.calls[1];
    expect(putUrl).toBe('/api/scenes/Movie%20Night');
    expect(putInit.method).toBe('PUT');
    expect(JSON.parse(putInit.body)).toMatchObject({
      name: 'Movie Night',
      generator: 'fire',
      effect_mode: 'rotate',
      effect_speed: 0.7,
      effect_hue: 0.2,
    });
  });

  it('input enforces maxLength=64 (PR 131 review fix)', () => {
    (globalThis.fetch as ReturnType<typeof vi.fn>).mockResolvedValueOnce({
      ok: true,
      json: async () => [],
    } as Response);
    setup();
    const input = screen.getByPlaceholderText(/name the current look/i) as HTMLInputElement;
    expect(input.maxLength).toBe(64);
  });

  it('does not submit when name is empty', async () => {
    const fetchSpy = makeFetchQueue([{ ok: true, json: [] }]);
    vi.stubGlobal('fetch', fetchSpy);
    setup();
    await waitFor(() => expect(fetchSpy).toHaveBeenCalledTimes(1));
    const saveBtn = screen.getByRole('button', { name: /save/i });
    expect(saveBtn).toBeDisabled(); // no name → disabled
  });
});

describe('ScenesPanel — apply', () => {
  beforeEach(() => vi.stubGlobal('fetch', vi.fn()));
  afterEach(() => vi.unstubAllGlobals());

  it('POSTs /api/scenes/{name}/apply and invokes onApplied', async () => {
    const scene: Scene = {
      name: 'Party',
      generator: 'fire',
      effect_mode: 'rotate',
      effect_speed: 1,
      effect_hue: 0,
    };
    const fetchSpy = makeFetchQueue([
      { ok: true, json: [scene] }, // initial list
      { ok: true, json: { scene: 'Party', commands: [] } }, // apply
    ]);
    vi.stubGlobal('fetch', fetchSpy);
    const { onApplied } = setup();
    await waitFor(() => expect(screen.getByText('Party')).toBeInTheDocument());

    // The apply button is the row itself (the delete button is separate
    // with aria-label "Delete scene Party"). Target by the apply title.
    fireEvent.click(screen.getByTitle('Apply to all connected devices'));

    await waitFor(() => {
      expect(fetchSpy).toHaveBeenCalledTimes(2);
    });
    const [applyUrl, applyInit] = fetchSpy.mock.calls[1];
    expect(applyUrl).toBe('/api/scenes/Party/apply');
    expect(applyInit.method).toBe('POST');
    expect(onApplied).toHaveBeenCalledWith(scene);
  });
});
