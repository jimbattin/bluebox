import { PAGE_KEY_SLOTS } from "../programs";

interface Props {
  pages: { page: number; title: string; available: boolean }[];
  selected: number;
  disabled: boolean;
  onSelect: (page: number) => void;
}

export function PageKeys({ pages, selected, disabled, onSelect }: Props) {
  return (
    <div class="keys page-keys">
      {pages.map((p) => (
        <button
          key={p.page}
          class={`key${p.page === selected ? " lit" : ""}`}
          disabled={disabled || !p.available}
          onClick={() => onSelect(p.page)}
        >
          <span class="led" />
          <span class="key-label">{p.title}</span>
        </button>
      ))}
      {/* blank keys fill the slots beyond the page keys, so the column keeps one height */}
      {Array.from({ length: Math.max(0, PAGE_KEY_SLOTS - pages.length) }, (_, i) => (
        <div key={`blank${i}`} class="key blank" aria-hidden="true" />
      ))}
    </div>
  );
}
