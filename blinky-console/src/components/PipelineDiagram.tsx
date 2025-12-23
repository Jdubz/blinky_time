/**
 * Simple static pipeline diagram showing the data flow architecture
 * Microphone → AdaptiveMic → Fire Generator → Effect → LEDs
 */

export function PipelineDiagram() {
  return (
    <div className="pipeline-diagram">
      <h3 className="pipeline-title">Processing Pipeline</h3>
      <div className="pipeline-flow">
        <div className="pipeline-stage">
          <div className="pipeline-icon">🎤</div>
          <div className="pipeline-label">Microphone</div>
        </div>

        <div className="pipeline-arrow">→</div>

        <div className="pipeline-stage pipeline-highlight">
          <div className="pipeline-icon">📊</div>
          <div className="pipeline-label">AdaptiveMic</div>
          <div className="pipeline-sublabel">Level • Transient • Envelope</div>
        </div>

        <div className="pipeline-arrow">→</div>

        <div className="pipeline-stage">
          <div className="pipeline-icon">🔥</div>
          <div className="pipeline-label">Fire Generator</div>
        </div>

        <div className="pipeline-arrow">→</div>

        <div className="pipeline-stage">
          <div className="pipeline-icon">🎨</div>
          <div className="pipeline-label">Effect</div>
          <div className="pipeline-sublabel">HueRotation</div>
        </div>

        <div className="pipeline-arrow">→</div>

        <div className="pipeline-stage">
          <div className="pipeline-icon">💡</div>
          <div className="pipeline-label">LEDs</div>
        </div>
      </div>
    </div>
  );
}
