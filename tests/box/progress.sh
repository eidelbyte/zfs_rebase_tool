# A progress display pinned to the terminal's bottom two rows while a
# long harness runs, sourced by the run-*.sh scripts:
#
#     <the harness's output, scrolling as it always did>
#     probe.zrt, the dataset form: dataset applying1 KILL
#     [##########################.........] 2/4 fixture forms 03:09
#
# The terminal's scroll region is set to every row but the last two,
# so the harness's own output keeps scrolling above and stays in the
# scrollback (the alternate screen would take that away, and the
# failures pasted from it with it). The label has a row of its own
# and the bar has the other, so neither is cut to make room for the
# other and the bar's width depends on the terminal's alone.
#
# Nothing is drawn unless stdout is a terminal, so a run sent to a
# file is plain text; ZR_PROGRESS=0 turns it off on a terminal and
# ZR_PROGRESS=1 forces it on. POSIX sh. The sequences are DECSTBM
# (the region), DECSC and DECRC (cursor save and restore), CUP and
# EL, which the FreeBSD console and every xterm-class terminal take.
# The size is read again at every draw, so a resized window is
# followed at the next step. Every function is safe to call when the
# display is off, before prog_start, or twice. While the display is
# on, SIGINT, SIGTERM and SIGHUP take it down and leave through exit,
# so the harness's EXIT trap and its teardown run on a Ctrl-C too.
#
#   prog_start TOTAL [UNITS]   begin: TOTAL units to come (0: count up)
#   prog_step [LABEL]          one unit done; LABEL is what is on now
#   prog_note [LABEL]          a new heading under the step's label
#   prog_end                   clear both rows and give the region back

prog_on=0
prog_total=0
prog_done=0
prog_units=steps
prog_label=""
prog_head=""
prog_t0=0
prog_rows=0
prog_cols=80

# The terminal's size: tput, else stty, else the environment, else a
# guess. Anything not a number is treated as unknown.
prog_size() {
	prog_rows=$(tput lines 2>/dev/null)
	prog_cols=$(tput cols 2>/dev/null)
	case "$prog_rows" in ''|*[!0-9]*)
		prog_rows=$(stty size 2>/dev/null | cut -d' ' -f1) ;;
	esac
	case "$prog_cols" in ''|*[!0-9]*)
		prog_cols=$(stty size 2>/dev/null | cut -d' ' -f2) ;;
	esac
	case "$prog_rows" in ''|*[!0-9]*) prog_rows=${LINES:-0} ;; esac
	case "$prog_cols" in ''|*[!0-9]*) prog_cols=${COLUMNS:-80} ;; esac
	case "$prog_rows" in ''|*[!0-9]*) prog_rows=0 ;; esac
	case "$prog_cols" in ''|*[!0-9]*) prog_cols=80 ;; esac
	[ "$prog_cols" -ge 20 ] || prog_cols=20
}

# One line of text made safe for the terminal: control characters
# out, cut to the width. Never longer than the row.
prog_clean() {
	printf '%s' "$1" | tr -d '\000-\037\177' | cut -c1-"$prog_cols" 2>/dev/null
}

prog_start() {
	[ "$prog_on" = 1 ] && prog_end
	prog_total=${1:-0}
	case "$prog_total" in ''|*[!0-9]*) prog_total=0 ;; esac
	prog_units=${2:-steps}
	prog_done=0
	prog_label=""
	prog_head=""
	prog_t0=$(date +%s 2>/dev/null || echo 0)
	case "${ZR_PROGRESS:-}" in
	0) return 0 ;;
	1) ;;
	*) [ -t 1 ] || return 0 ;;
	esac
	case "${TERM:-dumb}" in dumb|'') return 0 ;; esac
	prog_size
	[ "$prog_rows" -ge 6 ] || return 0
	prog_on=1
	# A signal that would kill the shell would leave the region set
	# and the display on the screen, and would skip the harness's
	# own EXIT trap with its teardown: take the display down and
	# leave through exit, which runs that trap.
	trap 'prog_end; exit 130' INT
	trap 'prog_end; exit 143' TERM
	trap 'prog_end; exit 129' HUP
	# Two rows of room, the cursor kept, the region shrunk, the
	# cursor put back and two rows up, so output goes on inside.
	printf '\n\n\0337\033[1;%dr\0338\033[2A' $((prog_rows - 2))
	prog_draw
}

prog_draw() {
	[ "$prog_on" = 1 ] || return 0
	prog_size
	if [ "$prog_rows" -lt 6 ]; then
		# Too small to hold the two rows: give the region back and
		# stay quiet until the window grows.
		printf '\033[r'
		return 0
	fi
	now=$(date +%s 2>/dev/null || echo 0)
	el=$((now - prog_t0))
	[ "$el" -ge 0 ] || el=0
	if [ "$prog_total" -gt 0 ]; then
		done=$prog_done
		[ "$done" -gt "$prog_total" ] && done=$prog_total
		text=$(printf ' %d/%d %s %02d:%02d' "$done" "$prog_total" \
		    "$prog_units" $((el / 60)) $((el % 60)))
		width=$(( prog_cols - ${#text} - 2 ))
		[ "$width" -ge 10 ] || width=10
		fill=$(( done * width / prog_total ))
		[ "$fill" -le "$width" ] || fill=$width
		bar=""
		i=0
		while [ $i -lt "$width" ]; do
			if [ $i -lt "$fill" ]; then bar="$bar#"; else bar="$bar."; fi
			i=$((i + 1))
		done
		line="[$bar]$text"
	else
		line=$(printf ' %d %s %02d:%02d' "$prog_done" "$prog_units" \
		    $((el / 60)) $((el % 60)))
	fi
	line=$(prog_clean "$line")
	if [ -n "$prog_head" ] && [ "$prog_head" != "$prog_label" ]; then
		label=$(prog_clean "$prog_label: $prog_head")
	else
		label=$(prog_clean "$prog_label")
	fi
	# The label's block ends a space after its text; the bar's runs
	# the width, which it fills anyway.
	label="$label "
	line=$(printf "%-${prog_cols}s" "$line")
	# Save the cursor; set the region again, since the window may
	# have been resized; the label row, then the bar row, each
	# cleared and written in reverse video; the cursor back.
	printf '\0337\033[1;%dr\033[%d;1H\033[2K\033[7m%s\033[0m\033[%d;1H\033[2K\033[7m%s\033[0m\0338' \
	    $((prog_rows - 2)) $((prog_rows - 1)) "$label" "$prog_rows" \
	    "$line"
}

prog_step() {
	prog_done=$((prog_done + 1))
	[ $# -ge 1 ] && prog_label=$1
	prog_head=""
	prog_draw
}

prog_note() {
	prog_head=${1:-}
	prog_draw
}

prog_end() {
	[ "$prog_on" = 1 ] || return 0
	prog_on=0
	trap - INT TERM HUP
	prog_size
	if [ "$prog_rows" -ge 2 ]; then
		printf '\0337\033[%d;1H\033[2K\033[%d;1H\033[2K\033[r\0338' \
		    $((prog_rows - 1)) "$prog_rows"
	else
		printf '\033[r'
	fi
}
