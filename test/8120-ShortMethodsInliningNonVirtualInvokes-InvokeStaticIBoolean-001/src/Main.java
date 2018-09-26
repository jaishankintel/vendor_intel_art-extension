class Main {
    final static int iterations = 10;
    
    public static void main(String[] args) {
        Test test = new Test();
        boolean nextThingy = false;

        System.out.println("Initial nextThingy value is " + nextThingy);

        for(int i = 0; i < iterations; i++) {
            nextThingy = !Test.getThingies(test);
            Test.setThingies(test, nextThingy);
        }

        System.out.println("Final nextThingy value is " + nextThingy);

    }
}
