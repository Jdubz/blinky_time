/**
 * Phase 5: Report Generation
 * Generates comprehensive reports from tuning results
 */

import { writeFileSync } from 'fs';
import { join } from 'path';
import type { DetectionMode } from './types.js';
import { DETECTION_MODES, PARAMETERS, ALL_PATTERNS } from './types.js';
import { StateManager } from './state.js';

export async function generateReport(
  outputDir: string,
  stateManager: StateManager
): Promise<void> {
  console.log('\n📝 Phase 5: Report Generation');
  console.log('═'.repeat(50));

  const reportsDir = join(outputDir, 'reports');

  // Generate summary report
  const summary = generateSummary(stateManager);
  writeFileSync(join(reportsDir, 'summary.md'), summary);
  console.log('✓ Generated summary.md');

  // Generate recommendations report
  const recommendations = generateRecommendations(stateManager);
  writeFileSync(join(reportsDir, 'recommendations.md'), recommendations);
  console.log('✓ Generated recommendations.md');

  // Generate detailed analysis
  const analysis = generateAnalysis(stateManager);
  writeFileSync(join(reportsDir, 'analysis.md'), analysis);
  console.log('✓ Generated analysis.md');

  stateManager.markDone();
  console.log('\n✅ Report generation complete.\n');
}

function generateSummary(stateManager: StateManager): string {
  const lines: string[] = [];

  lines.push('# Parameter Tuning Results');
  lines.push('');
  lines.push(`Generated: ${new Date().toISOString()}`);
  lines.push('');

  // Executive summary
  lines.push('## Executive Summary');
  lines.push('');

  // Find best algorithm
  let bestMode: DetectionMode | null = null;
  let bestF1 = 0;
  let bestBaseline = 0;

  for (const mode of DETECTION_MODES) {
    const result = stateManager.getValidationResult(mode);
    const baseline = stateManager.getBaselineResult(mode);
    if (result && result.overall.f1 > bestF1) {
      bestF1 = result.overall.f1;
      bestMode = mode;
      bestBaseline = baseline?.overall.avgF1 ?? 0;
    }
  }

  if (bestMode) {
    const improvement = Math.round((bestF1 - bestBaseline) * 100);
    lines.push(`- **Best overall algorithm:** ${bestMode.toUpperCase()} (F1: ${bestF1})`);
    lines.push(`- **Improvement over defaults:** ${improvement > 0 ? '+' : ''}${improvement}% F1 score`);
  }

  lines.push('');

  // Optimal parameters table for each mode
  lines.push('## Optimal Parameters');
  lines.push('');

  for (const mode of DETECTION_MODES) {
    const validation = stateManager.getValidationResult(mode);
    const baseline = stateManager.getBaselineResult(mode);

    lines.push(`### ${mode.charAt(0).toUpperCase() + mode.slice(1)} Algorithm`);
    lines.push('');

    if (!validation) {
      lines.push('*Not yet validated*');
      lines.push('');
      continue;
    }

    lines.push('| Parameter | Default | Optimal | Change |');
    lines.push('|-----------|---------|---------|--------|');

    const modeParams = Object.values(PARAMETERS).filter(p => p.mode === mode);
    for (const param of modeParams) {
      const def = param.default;
      const opt = validation.params[param.name] ?? def;
      const change = opt !== def
        ? `${opt > def ? '+' : ''}${Math.round((opt - def) / def * 100)}%`
        : '-';
      lines.push(`| ${param.name} | ${def} | ${opt} | ${change} |`);
    }

    lines.push('');

    if (baseline && validation) {
      const delta = validation.vsBaseline.f1Delta;
      lines.push(`**Performance:** F1 ${validation.overall.f1} (${delta > 0 ? '+' : ''}${delta} vs baseline)`);
      lines.push('');
    }
  }

  // Pattern-specific insights
  lines.push('## Pattern-Specific Insights');
  lines.push('');

  const patternBest: Record<string, { mode: DetectionMode; f1: number }> = {};

  for (const mode of DETECTION_MODES) {
    const validation = stateManager.getValidationResult(mode);
    if (!validation) continue;

    for (const [pattern, result] of Object.entries(validation.patterns)) {
      if (!patternBest[pattern] || result.f1 > patternBest[pattern].f1) {
        patternBest[pattern] = { mode, f1: result.f1 };
      }
    }
  }

  // Group patterns by best mode
  const byMode: Record<DetectionMode, string[]> = {
    drummer: [],
    bass: [],
    hfc: [],
    spectral: [],
    hybrid: [],
  };

  for (const [pattern, { mode }] of Object.entries(patternBest)) {
    byMode[mode].push(pattern);
  }

  for (const mode of DETECTION_MODES) {
    if (byMode[mode].length > 0) {
      lines.push(`- **${mode.charAt(0).toUpperCase() + mode.slice(1)}** excels on: ${byMode[mode].join(', ')}`);
    }
  }

  lines.push('');

  return lines.join('\n');
}

function generateRecommendations(stateManager: StateManager): string {
  const lines: string[] = [];

  lines.push('# Recommended Settings by Use Case');
  lines.push('');
  lines.push(`Generated: ${new Date().toISOString()}`);
  lines.push('');

  // Find modes with best precision vs recall trade-off
  let highPrecisionMode: DetectionMode | null = null;
  let highRecallMode: DetectionMode | null = null;
  let highPrecision = 0;
  let highRecall = 0;
  let bestOverall: DetectionMode | null = null;
  let bestF1 = 0;

  for (const mode of DETECTION_MODES) {
    const validation = stateManager.getValidationResult(mode);
    if (!validation) continue;

    if (validation.overall.precision > highPrecision) {
      highPrecision = validation.overall.precision;
      highPrecisionMode = mode;
    }
    if (validation.overall.recall > highRecall) {
      highRecall = validation.overall.recall;
      highRecallMode = mode;
    }
    if (validation.overall.f1 > bestF1) {
      bestF1 = validation.overall.f1;
      bestOverall = mode;
    }
  }

  // Live Performance (low false positives = high precision)
  lines.push('## Live Performance (Low Latency, Few False Triggers)');
  lines.push('');
  if (highPrecisionMode) {
    const validation = stateManager.getValidationResult(highPrecisionMode);
    lines.push(`- **Mode:** ${highPrecisionMode}`);
    lines.push(`- **Why:** Highest precision (${validation?.overall.precision}) = fewer false triggers`);
    lines.push('- **Settings:**');
    if (validation) {
      for (const [param, value] of Object.entries(validation.params)) {
        lines.push(`  - \`set ${param} ${value}\``);
      }
    }
  }
  lines.push('');

  // Studio Monitoring (best overall accuracy)
  lines.push('## Studio Monitoring (Best Overall Accuracy)');
  lines.push('');
  if (bestOverall) {
    const validation = stateManager.getValidationResult(bestOverall);
    lines.push(`- **Mode:** ${bestOverall}`);
    lines.push(`- **Why:** Highest F1 score (${validation?.overall.f1}) = best balance`);
    lines.push('- **Settings:**');
    if (validation) {
      for (const [param, value] of Object.entries(validation.params)) {
        lines.push(`  - \`set ${param} ${value}\``);
      }
    }
  }
  lines.push('');

  // Catch Everything (high recall)
  lines.push('## Catch Everything (High Sensitivity)');
  lines.push('');
  if (highRecallMode) {
    const validation = stateManager.getValidationResult(highRecallMode);
    lines.push(`- **Mode:** ${highRecallMode}`);
    lines.push(`- **Why:** Highest recall (${validation?.overall.recall}) = catches most transients`);
    lines.push('- **Settings:**');
    if (validation) {
      for (const [param, value] of Object.entries(validation.params)) {
        lines.push(`  - \`set ${param} ${value}\``);
      }
    }
  }
  lines.push('');

  // Quick reference
  lines.push('## Quick Reference Commands');
  lines.push('');

  for (const mode of DETECTION_MODES) {
    const validation = stateManager.getValidationResult(mode);
    if (!validation) continue;

    lines.push(`### ${mode.charAt(0).toUpperCase() + mode.slice(1)}`);
    lines.push('```');
    lines.push(`set detectmode ${mode === 'drummer' ? 0 : mode === 'spectral' ? 3 : 4}`);
    for (const [param, value] of Object.entries(validation.params)) {
      lines.push(`set ${param} ${value}`);
    }
    lines.push('save');
    lines.push('```');
    lines.push('');
  }

  return lines.join('\n');
}

function generateAnalysis(stateManager: StateManager): string {
  const lines: string[] = [];

  lines.push('# Detailed Analysis');
  lines.push('');
  lines.push(`Generated: ${new Date().toISOString()}`);
  lines.push('');

  // Sweep analysis
  lines.push('## Parameter Sensitivity Analysis');
  lines.push('');

  for (const param of Object.values(PARAMETERS)) {
    const sweep = stateManager.getSweepResult(param.name);
    if (!sweep) continue;

    lines.push(`### ${param.name} (${param.mode})`);
    lines.push('');
    lines.push(`*${param.description}*`);
    lines.push('');
    lines.push('| Value | F1 | Precision | Recall |');
    lines.push('|-------|-----|-----------|--------|');

    for (const point of sweep.sweep) {
      const marker = point.value === sweep.optimal.value ? ' **⭐**' : '';
      lines.push(`| ${point.value}${marker} | ${point.avgF1} | ${point.avgPrecision} | ${point.avgRecall} |`);
    }
    lines.push('');

    // Analysis notes
    if (sweep.sweep.length > 1) {
      const first = sweep.sweep[0];
      const last = sweep.sweep[sweep.sweep.length - 1];

      if (first.avgPrecision < last.avgPrecision && first.avgRecall > last.avgRecall) {
        lines.push('*Trend: Higher values favor precision over recall.*');
      } else if (first.avgPrecision > last.avgPrecision && first.avgRecall < last.avgRecall) {
        lines.push('*Trend: Higher values favor recall over precision.*');
      }
      lines.push('');
    }
  }

  // Interaction analysis
  lines.push('## Parameter Interactions');
  lines.push('');

  const hybridWeights = stateManager.getInteractionResult('hybrid-weights');
  if (hybridWeights) {
    lines.push('### Hybrid Weight Balance');
    lines.push('');
    lines.push('Best combinations:');
    lines.push('');

    // Sort by F1 and show top 5
    const sorted = [...hybridWeights.grid].sort((a, b) => b.avgF1 - a.avgF1).slice(0, 5);
    for (const point of sorted) {
      const params = Object.entries(point.params).map(([k, v]) => `${k}=${v}`).join(', ');
      lines.push(`- ${params} → F1: ${point.avgF1}`);
    }
    lines.push('');
  }

  // Pattern breakdown
  lines.push('## Pattern Performance Breakdown');
  lines.push('');
  lines.push('| Pattern | Drummer | Spectral | Hybrid | Best |');
  lines.push('|---------|---------|----------|--------|------|');

  const patterns = ALL_PATTERNS as unknown as string[];
  for (const pattern of patterns) {
    const drummerV = stateManager.getValidationResult('drummer');
    const spectralV = stateManager.getValidationResult('spectral');
    const hybridV = stateManager.getValidationResult('hybrid');

    const d = drummerV?.patterns[pattern]?.f1 ?? '-';
    const s = spectralV?.patterns[pattern]?.f1 ?? '-';
    const h = hybridV?.patterns[pattern]?.f1 ?? '-';

    const values = [
      { mode: 'drummer', f1: typeof d === 'number' ? d : 0 },
      { mode: 'spectral', f1: typeof s === 'number' ? s : 0 },
      { mode: 'hybrid', f1: typeof h === 'number' ? h : 0 },
    ].filter(v => v.f1 > 0);

    const best = values.length > 0
      ? values.reduce((a, b) => a.f1 > b.f1 ? a : b).mode
      : '-';

    lines.push(`| ${pattern} | ${d} | ${s} | ${h} | ${best} |`);
  }

  lines.push('');

  return lines.join('\n');
}

export function showReportSummary(outputDir: string): void {
  console.log('\n📝 Reports Generated');
  console.log('═'.repeat(50));
  console.log(`\nReports saved to: ${join(outputDir, 'reports')}`);
  console.log('  - summary.md: Executive summary and optimal parameters');
  console.log('  - recommendations.md: Use-case specific settings');
  console.log('  - analysis.md: Detailed parameter sensitivity analysis');
}
