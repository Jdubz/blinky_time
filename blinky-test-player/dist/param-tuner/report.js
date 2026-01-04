/**
 * Report Generation
 * Generates summary reports from tuning results
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Reports on ensemble detection performance.
 * Legacy per-mode reporting has been removed.
 */
import { writeFileSync, readFileSync, existsSync, mkdirSync } from 'fs';
import { join } from 'path';
import { PARAMETERS, ALL_PATTERNS } from './types.js';
export async function generateReport(optionsOrOutputDir, stateManager) {
    console.log('\n Generating Report');
    console.log('='.repeat(50));
    const outputDir = typeof optionsOrOutputDir === 'string'
        ? optionsOrOutputDir
        : (optionsOrOutputDir.outputDir || 'tuning-results');
    const reportsDir = join(outputDir, 'reports');
    if (!existsSync(reportsDir)) {
        mkdirSync(reportsDir, { recursive: true });
    }
    // Generate markdown report
    const lines = [];
    lines.push('# Ensemble Tuning Report');
    lines.push('');
    lines.push(`Generated: ${new Date().toISOString()}`);
    lines.push('');
    // Executive Summary
    lines.push('## Executive Summary');
    lines.push('');
    const baseline = stateManager.getBaselineResult();
    const validation = stateManager.getValidationResult();
    const optimalParams = stateManager.getOptimalParams();
    if (baseline) {
        lines.push(`**Baseline F1:** ${baseline.overall.avgF1}`);
    }
    if (validation) {
        lines.push(`**Optimized F1:** ${validation.overall.f1}`);
        lines.push(`**Improvement:** ${validation.vsBaseline.f1Delta > 0 ? '+' : ''}${validation.vsBaseline.f1Delta}`);
    }
    lines.push('');
    // Optimal Parameters
    if (optimalParams && Object.keys(optimalParams).length > 0) {
        lines.push('## Optimal Parameters');
        lines.push('');
        lines.push('```');
        for (const [param, value] of Object.entries(optimalParams)) {
            const def = PARAMETERS[param]?.default;
            const change = value !== def ? ` (default: ${def})` : '';
            lines.push(`${param}: ${value}${change}`);
        }
        lines.push('```');
        lines.push('');
        // Serial commands to apply
        lines.push('### Apply to Device');
        lines.push('');
        lines.push('```');
        for (const [param, value] of Object.entries(optimalParams)) {
            const paramDef = PARAMETERS[param];
            if (paramDef?.command) {
                lines.push(`set ${paramDef.command} ${value}`);
            }
            else {
                lines.push(`set ${param} ${value}`);
            }
        }
        lines.push('save');
        lines.push('```');
        lines.push('');
    }
    // Per-Pattern Results
    if (validation?.patterns) {
        lines.push('## Per-Pattern Results');
        lines.push('');
        lines.push('| Pattern | F1 | Precision | Recall | vs Baseline |');
        lines.push('|---------|-----|-----------|--------|-------------|');
        for (const pattern of ALL_PATTERNS) {
            const result = validation.patterns[pattern];
            const baseResult = baseline?.patterns[pattern];
            if (result) {
                const delta = baseResult
                    ? (result.f1 - baseResult.f1).toFixed(3)
                    : 'N/A';
                const deltaStr = baseResult
                    ? (result.f1 > baseResult.f1 ? `+${delta}` : delta)
                    : 'N/A';
                lines.push(`| ${pattern} | ${result.f1} | ${result.precision} | ${result.recall} | ${deltaStr} |`);
            }
        }
        lines.push('');
    }
    // Improved and Regressed
    if (validation?.vsBaseline) {
        if (validation.vsBaseline.improved.length > 0) {
            lines.push('### Improved Patterns');
            lines.push('');
            for (const pattern of validation.vsBaseline.improved) {
                lines.push(`- ${pattern}`);
            }
            lines.push('');
        }
        if (validation.vsBaseline.regressed.length > 0) {
            lines.push('### Regressed Patterns');
            lines.push('');
            for (const pattern of validation.vsBaseline.regressed) {
                lines.push(`- ${pattern}`);
            }
            lines.push('');
        }
    }
    // Sweep Results
    const state = stateManager.getState();
    if (state.sweeps?.results && Object.keys(state.sweeps.results).length > 0) {
        lines.push('## Sweep Results');
        lines.push('');
        for (const [param, sweep] of Object.entries(state.sweeps.results)) {
            lines.push(`### ${param}`);
            lines.push('');
            lines.push(`- Optimal: ${sweep.optimal.value} (F1: ${sweep.optimal.avgF1})`);
            lines.push(`- Sweep: ${sweep.sweep.map(p => `${p.value}=${p.avgF1}`).join(', ')}`);
            lines.push('');
        }
    }
    // Save report
    const report = lines.join('\n');
    const reportPath = join(reportsDir, 'ensemble-tuning-report.md');
    writeFileSync(reportPath, report);
    console.log(`Report saved to: ${reportPath}`);
    // Also save JSON summary
    const jsonSummary = {
        timestamp: new Date().toISOString(),
        baseline: baseline?.overall,
        validation: validation?.overall,
        improvement: validation?.vsBaseline?.f1Delta,
        optimalParams,
        patternsImproved: validation?.vsBaseline?.improved?.length ?? 0,
        patternsRegressed: validation?.vsBaseline?.regressed?.length ?? 0,
    };
    const jsonPath = join(reportsDir, 'ensemble-summary.json');
    writeFileSync(jsonPath, JSON.stringify(jsonSummary, null, 2));
    console.log(`JSON summary saved to: ${jsonPath}`);
    stateManager.markDone();
    console.log('\n Report generation complete.\n');
}
/**
 * Show a summary of the generated report
 */
export function showReportSummary(outputDir) {
    const reportsDir = join(outputDir, 'reports');
    const jsonPath = join(reportsDir, 'ensemble-summary.json');
    console.log('\n Report Summary');
    console.log('='.repeat(50));
    if (!existsSync(jsonPath)) {
        console.log('No report found. Run the report phase first.');
        return;
    }
    try {
        const summary = JSON.parse(readFileSync(jsonPath, 'utf-8'));
        console.log(`\nTimestamp: ${summary.timestamp}`);
        if (summary.baseline) {
            console.log(`Baseline F1: ${summary.baseline.avgF1}`);
        }
        if (summary.validation) {
            console.log(`Optimized F1: ${summary.validation.f1}`);
        }
        if (summary.improvement !== undefined) {
            const sign = summary.improvement > 0 ? '+' : '';
            console.log(`Improvement: ${sign}${summary.improvement}`);
        }
        if (summary.optimalParams && Object.keys(summary.optimalParams).length > 0) {
            console.log('\nOptimal Parameters:');
            for (const [param, value] of Object.entries(summary.optimalParams)) {
                console.log(`  ${param}: ${value}`);
            }
        }
        console.log(`\nPatterns Improved: ${summary.patternsImproved}`);
        console.log(`Patterns Regressed: ${summary.patternsRegressed}`);
        const reportPath = join(reportsDir, 'ensemble-tuning-report.md');
        console.log(`\nFull report: ${reportPath}`);
    }
    catch (err) {
        console.error('Failed to read report summary:', err);
    }
}
