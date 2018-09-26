
public class Main {

    // Test a loop with invariants, which should be hoisted (Constant Calculation Sinking)
    // IV and constants are also used in the loop. 

    public int loop() {
        int used1 = 1;
        int used2 = 2;
        int used3 = 3;
        int used4 = 4;

        int invar1 = 15;
        int invar2 = 25;
        int invar3 = 35;
        int invar4 = 45;
        for (int i = 0; i < 10000; i++) {
            used1 += invar1 + invar2;
            used2 -= invar2 - invar3;
            used3 /= invar3 * invar4;
            used4 %= invar1 * invar2 - invar3 + invar4;
        }
        used1 += used4;
        return used1;
    }

    public static void main(String[] args) {
        int res = new Main().loop();
        System.out.println(res);
    }
}
