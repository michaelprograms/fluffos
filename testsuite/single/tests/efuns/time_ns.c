void do_tests() {
  int x = time_ns();
  write("\n\ntime_ns is: " + x + "   ("+(x > 1685382080000000)+")\n\n");
  ASSERT(x > 1685382080000000);
}
