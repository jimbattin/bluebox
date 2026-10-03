import { PROGRAMS } from "../programs";

const BANK_SIZE = 6;
const BANKS = 4;

interface Props {
  current: number;
  disabled: boolean;
  onSelect: (index: number) => void;
}

export function ProgramKeypad({ current, disabled, onSelect }: Props) {
  const banks = [];
  for (let b = 0; b < BANKS; b++) {
    const first = b * BANK_SIZE;
    const keys = PROGRAMS.slice(first, first + BANK_SIZE).map((prog, n) => (
      <button
        key={first + n}
        class={`key program-key${first + n === current ? " lit" : ""}`}
        disabled={disabled}
        onClick={() => onSelect(first + n)}
      >
        <span class="led" />
        <span class="key-num">{n + 1}</span>
        <span class="key-label">{prog.name}</span>
      </button>
    ));
    banks.push(
      <div class="bank" key={b}>
        <div class="silk bank-title">BANK {b + 1}</div>
        {keys}
      </div>,
    );
  }
  return <section class="keypad">{banks}</section>;
}
