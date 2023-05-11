describe("Encrypt String", function () {
    it("returns the next character", function () {
        var expectations = {
            'a': 'b', 'b': 'c', 'c': 'd', 'd': 'e', 'e': 'f',
            'f': 'g', 'g': 'h', 'h': 'i', 'i': 'j', 'j': 'k',
            'k': 'l', 'l': 'm', 'm': 'n', 'n': 'o', 'o': 'p',
            'p': 'q', 'q': 'r', 'r': 's', 's': 't', 't': 'u',
            'u': 'v', 'v': 'w', 'w': 'x', 'x': 'y', 'y': 'z',
            'z': 'a',
            'A': 'B', 'B': 'C', 'C': 'D', 'D': 'E', 'E': 'F',
            'F': 'G', 'G': 'H', 'H': 'I', 'I': 'J', 'J': 'K',
            'K': 'L', 'L': 'M', 'M': 'N', 'N': 'O', 'O': 'P',
            'P': 'Q', 'Q': 'R', 'R': 'S', 'S': 'T', 'T': 'U',
            'U': 'V', 'V': 'W', 'W': 'X', 'X': 'Y', 'Y': 'Z',
            'Z': 'A'
        };

        for (var property in expectations) {
            if (expectations.hasOwnProperty(property)) {
                var charToEncrypt = property;
                var expectedEncryptedChar = expectations[property];

                expect(encrypt(charToEncrypt)).toEqual(expectedEncryptedChar);
            }
        }
    });

    it("when input string is 'television' it returns 'ufmfwjtjpn'", function(){
        expect(encrypt('television')).toEqual('ufmfwjtjpo');
    });

    it("can encrypt a word", function(){
        expect(encrypt('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ')).
        toEqual('bcdefghijklmnopqrstuvwxyzaBCDEFGHIJKLMNOPQRSTUVWXYZA');
    });

    it("throws an exception if there is any character that does not belong to the latin alphabet", function() {
        expect(function(){
            encrypt("This includes the blank character that does not belong to latin alphabet")
        }).toThrowError(ArgumentError, "non-latin alphabet character encountered");
    });

});

