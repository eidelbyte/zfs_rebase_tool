/* The built-in picker: the child -i runs when it names no command. */

#ifndef	ZR_PICKER_H
#define	ZR_PICKER_H

/*
 * The entry the launcher calls in the forked child
 * (sprints/sprint-6/implementation-plan.md, section 3.2), with the
 * argv zr_launch_argv builds:
 *
 *	zfs_rebase-picker RESOLUTION BASE FROM ONTO RESULT
 *
 * where the last four are the directories the trees are at and "" is
 * a tree there is no path for. The status it returns is the child's:
 * 0 the document written with no name left unanswered, so the tool
 * goes on; 1 saved and stop, the answers so far written and the gate
 * left standing; 2 abandoned with nothing written, which is also
 * what a picker that cannot open the terminal exits with. To the
 * tool anything but 0 is one thing -- the gate stands -- and the
 * picker's own three meanings are the person's.
 *
 * The picker depends on the documents and the name codec and on
 * nothing else of the tool: never on the driver, never on the ZFS
 * layer (ground rule 1). It could be lifted out whole.
 */
int zr_picker_main(int argc, char **argv);

#endif	/* ZR_PICKER_H */
