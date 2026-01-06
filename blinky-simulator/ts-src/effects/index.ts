export { HueRotationEffect, HueRotationParams } from './HueRotation';

import { Effect } from '../types';
import { HueRotationEffect } from './HueRotation';

export type EffectType = 'none' | 'hue';

class NoOpEffect implements Effect {
  name = 'NoOp';

  begin(_width: number, _height: number): void {}
  apply(_matrix: any): void {}
  reset(): void {}
}

export function createEffect(type: EffectType): Effect {
  switch (type) {
    case 'hue':
      return new HueRotationEffect();
    case 'none':
    default:
      return new NoOpEffect();
  }
}
