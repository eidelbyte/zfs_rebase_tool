# A progress bar pinned to the terminal's bottom row while a long
# harness runs, sourced by the run-*.sh scripts. The terminal's scroll
# region is set to every row but the last, so the harness's own
# output keeps scrolling above and stays in the scrollback (the
# alternate screen would take that away, and the failures pasted
# from it with it); the last row holds the bar, redrawn on every
# step. Nothing is drawn unless stdout is a terminal, so a run sent
# to a file is plain text; ZR_PROGRESS=0 turns it off on a terminal
# and ZR_PROGRESS=1 forces it on. POSIX sh; the sequences are
# DECSTBM (region), DECSC/DECRC (cursor save and restore), CUP and
# EL, which the FreeBSD console and every xterm-class terminal take.
#
#   prog_start TOTAL [UNITS]   begin: TOTAL units to come
#   prog_step LABEL            one unit done, LABEL is what is on now
#   prog_note LABEL            a new label, no unit done (a heading)
#   prog_end                   clear the bar and give the region back

prog_on=0
prog_total=0
prog_done=0
prog_units=steps
prog_label=""
prog_t0=0
prog_rows=0

prog_size() {
	prog_rows=$(tput lines 2>/dev/null)
	prog_cols=$(tput cols 2>/dev/null)
	case "$prog_rows" in ''|*[!0-9]*) prog_rows=0 ;; esac
	case "$prog_cols" in ''|*[!0-9]*) prog_cols=80 ;; esac
}

prog_start() {
	prog_total=${1:-0}
	prog_units=${2:-steps}
	prog_done=0
	prog_label=""
	prog_t0=$(date +%s)
	case "${ZR_PROGRESS:-}" in
	0) return 0 ;;
	1) ;;
	*) [ -t 1 ] || return 0 ;;
	esac
	case "${TERM:-dumb}" in dumb|'') return 0 ;; esac
	prog_size
	[ "$prog_rows" -ge 4 ] || return 0
	prog_on=1
	# Make room, keep the cursor, shrink the region, put the cursor
	# back and one row up, so that output goes on inside the region.
	printf '\n\0337\033[1;%dr\0338\033[1A' $((prog_rows - 1))
	prog_draw
}

prog_draw() {
	[ "$prog_on" = 1 ] || return 0
	prog_size
	[ "$prog_rows" -ge 4 ] || return 0
	el=$(( $(date +%s) - prog_t0 ))
	if [ "$prog_total" -gt 0 ]; then
		fill=$(( prog_done * 20 / prog_total ))
		[ "$fill" -gt 20 ] && fill=20
		bar=""
		i=0
		while [ $i -lt 20 ]; do
			if [ $i -lt "$fill" ]; then bar="$bar#"; else bar="$bar."; fi
			i=$((i + 1))
		done
		head=$(printf '[%s] %d/%d %s %02d:%02d ' "$bar" "$prog_done" \
		    "$prog_total" "$prog_units" $((el / 60)) $((el % 60)))
	else
		head=$(printf '%d %s %02d:%02d ' "$prog_done" "$prog_units" \
		    $((el / 60)) $((el % 60)))
	fi
	room=$(( prog_cols - ${#head} - 1 ))
	[ "$room" -lt 0 ] && room=0
	tail=$(printf '%s' "$prog_label" | cut -c1-"$room" 2>/dev/null)
	# Save the cursor, set the region (again: the window may have
	# been resized), go to the last row, clear it, draw, restore.
	printf '\0337\033[1;%dr\033[%d;1H\033[2K\033[7m%s%s\033[0m\0338' \
	    $((prog_rows - 1)) "$prog_rows" "$head" "$tail"
}

prog_step() {
	prog_done=$((prog_done + 1))
	prog_label=${1:-}
	prog_draw
}

prog_note() {
	[ "$prog_on" = 1 ] || return 0
	prog_label="${prog_label%%: *}"
	prog_label="$prog_label: ${1:-}"
	prog_draw
}

prog_end() {
	[ "$prog_on" = 1 ] || return 0
	prog_on=0
	prog_size
	printf '\0337\033[%d;1H\033[2K\033[r\0338' "$prog_rows"
}
