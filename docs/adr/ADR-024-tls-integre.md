# ADR-024 — TLS intégré

**Statut** : proposé ([#631](https://github.com/ambroise-leclerc/WebFront/issues/631)).

Un fournisseur réseau optionnel termine TLS 1.3 avant de livrer le flux au serveur HTTP et au pont WebSocket.
Le cœur HTTP garde son fournisseur TCP. Un port TLS sert exclusivement HTTPS et WSS avec HTTP/1.1.
Le profil initial utilise X25519 (avec HelloRetryRequest), ChaCha20-Poly1305, SHA-256 et ECDSA P-256
avec nonce RFC 6979. AES-GCM reste conditionné à une implémentation à temps constant raisonnable.
La reprise par tickets ajouterait une politique de rotation, une durée de vie et un second chemin de handshake.
Le premier profil fait un handshake complet à chaque connexion.

L'application fournit une chaîne PEM et une clé P-256 via la configuration. Le rechargement explicite
publie une paire complète après validation. Les handshakes en cours conservent leur instantané.
Le navigateur valide la chaîne et le nom du serveur. TLS protège le transport, tandis que la politique
d'origine continue de décider de l'accès au pont. Le profil ne demande aucun certificat au client.
Le serveur sait que le transport est chiffré lorsqu'il termine TLS et marque alors le cookie de liaison `Secure`.

Le composant demeure header-only, sous `include/webfront/tls/`. Il réutilise des primitives compactes
avec provenance et licences conservées. La génération de secrets utilise l'aléa de l'OS. Les opérations
sur des secrets exigent des accès mémoire et un contrôle de flux indépendants de ces secrets, avec
vérification sur les compilateurs et architectures supportés. Les vecteurs des RFC, les refus de protocole,
le fuzzing et un test navigateur HTTPS/WSS font partie de la qualification. Les régressions de sécurité
peuvent être signalées en privé aux mainteneurs. La maintenance du composant inclut le suivi des correctifs
amont des primitives et des mesures reproductibles de taille et de mémoire.
