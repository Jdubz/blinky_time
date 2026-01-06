export { FireGenerator, FireParams } from './Fire';
export { WaterGenerator, WaterParams } from './Water';
export { LightningGenerator, LightningParams } from './Lightning';

import { Generator } from '../types';
import { FireGenerator } from './Fire';
import { WaterGenerator } from './Water';
import { LightningGenerator } from './Lightning';

export type GeneratorType = 'fire' | 'water' | 'lightning';

export function createGenerator(type: GeneratorType): Generator {
  switch (type) {
    case 'fire':
      return new FireGenerator();
    case 'water':
      return new WaterGenerator();
    case 'lightning':
      return new LightningGenerator();
    default:
      return new FireGenerator();
  }
}
