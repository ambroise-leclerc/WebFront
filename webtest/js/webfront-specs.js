describe("WebFront connection", function() {
    it("returns its version", function () {
        let getVersion = webFront.cppFunction('getVersion');
        expect(getVersion() == "0.0.1");
    });
});
