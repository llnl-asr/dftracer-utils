import type { FlameNode } from "../data/types";
import { colorFor } from "../timeline/color";
import { formatTime } from "../timeline/format";

export interface FlameHover {
  node: FlameNode;
  pct: number;
  x: number;
  y: number;
}

export function FlameTooltip(props: { hover: FlameHover }) {
  const n = () => props.hover.node;
  return (
    <div
      class="tooltip"
      style={{ left: `${props.hover.x + 14}px`, top: `${props.hover.y + 14}px` }}
    >
      <div class="tt-name">
        <i style={{ background: colorFor(n().name) }} />
        {n().name}
      </div>
      <div class="tt-row">
        <span>total</span>
        <b>
          {formatTime(n().total)} ({props.hover.pct.toFixed(1)}%)
        </b>
      </div>
      <div class="tt-row">
        <span>self</span>
        <b>{formatTime(n().self)}</b>
      </div>
      <div class="tt-row">
        <span>count</span>
        <b>{n().count.toLocaleString()}</b>
      </div>
    </div>
  );
}
