int called;
int xverb;
int utf8verb;

void do_tests() {
#ifndef __NO_ADD_ACTION__
    object tp;
    string long_missing_verb;
    mixed err;
    xverb = called = utf8verb = 0;
    SAVETP;
    enable_commands();
    add_action( (: called = 1 :), "foo");
    add_action( (: xverb = 1 :), "b", 1);
    add_action( (: utf8verb = 1 :), "测试测试");
    long_missing_verb = repeat_string("v", 240) + "%s";
    add_action("missing_action_handler", long_missing_verb);
    RESTORETP;
    command("foo");
    command("bar");
    command("测试测试");
    err = catch(command(long_missing_verb));
    ASSERT_EQ(1, called);
    ASSERT_EQ(1, xverb);
    ASSERT_EQ(1, utf8verb);
    ASSERT_EQ(sprintf("*Function for verb '%s' not found.\n", long_missing_verb), err);
#endif
}
