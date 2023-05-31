void do_tests() {
  int x = time_ns();
  write("\n\n"+sprintf("%-20s: time_ns == %d", __ARCH__, x)+"\n\n");
  ASSERT(x > 1685382080000000);
}
