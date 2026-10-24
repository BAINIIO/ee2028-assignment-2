set pagination off
target extended-remote localhost:3333
monitor reset halt
break main
echo Connected. Use 'load' to program this ELF, then 'continue' to run.\n
