describe("WebFront Framework Tests", function() {
    
    describe("C++ to JavaScript Bridge", function() {
        it("should retrieve WebFront version from C++", function () {
            let getVersion = webFront.cppFunction('getVersion');
            expect(getVersion()).toBe("0.0.1");
        });

        it("should handle function calls with parameters", function () {
            let getVersion = webFront.cppFunction('getVersion');
            expect(getVersion("test-param")).toBe("0.0.1");
        });

        it("should throw error for non-existent C++ function", function () {
            expect(function() {
                webFront.cppFunction('nonExistentFunction');
            }).toThrow();
        });
    });

    describe("WebFront Object", function() {
        it("should be available globally", function () {
            expect(webFront).toBeDefined();
            expect(typeof webFront).toBe("object");
        });

        it("should have cppFunction method", function () {
            expect(webFront.cppFunction).toBeDefined();
            expect(typeof webFront.cppFunction).toBe("function");
        });
    });

    describe("Type Safety", function() {
        it("should handle string parameters correctly", function () {
            let getVersion = webFront.cppFunction('getVersion');
            expect(function() {
                getVersion("test");
            }).not.toThrow();
        });

        it("should handle empty string parameters", function () {
            let getVersion = webFront.cppFunction('getVersion');
            expect(function() {
                getVersion("");
            }).not.toThrow();
        });
    });
});
