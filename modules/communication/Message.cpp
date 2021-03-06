#ifdef UNIT_TESTING

#include "catch.hpp"

#endif

#include "Message.hpp"

#include <utility>

namespace ProtoMesh::communication {

    Result<vector<uint8_t>, Message::MessageDecryptionError>
    Message::decryptPayload(cryptography::asymmetric::PublicKey sender, cryptography::asymmetric::KeyPair recipient) {

        /// Calculate the shared secret
        vector<uint8_t> sharedSecret = cryptography::asymmetric::generateSharedSecret(sender, recipient.priv);

        /// Decrypt the value
        vector<uint8_t> decryptedPayload = cryptography::symmetric::decrypt(this->payload, sharedSecret);

        /// Validate the signature
        if (!cryptography::asymmetric::verify(decryptedPayload, this->signature, &sender))
            return Err(MessageDecryptionError::InvalidSignature);

        return Ok(decryptedPayload);
    }

    vector<uint8_t> Message::serialize() const {

        using namespace scheme::communication;
        flatbuffers::FlatBufferBuilder builder;

        /// Serialize the route
        vector<scheme::cryptography::UUID> routeEntries;
        for (auto hop : this->route)
            routeEntries.push_back(hop.toScheme());

        auto routeVector = builder.CreateVectorOfStructs(routeEntries);

        /// Serialize the payload
        auto payload = builder.CreateVector(this->payload);

        /// Serialize the signature
        // TODO Make this more memory efficient.
        vector<uint8_t> signatureVector(this->signature.begin(), this->signature.end());
        auto signature = builder.CreateVector(signatureVector);

        auto message = CreateMessageDatagram(builder, routeVector, payload, signature);

        /// Convert it to a byte array
        builder.Finish(message, MessageDatagramIdentifier());
        uint8_t *buf = builder.GetBufferPointer();

        return {buf, buf + builder.GetSize()};
    }

    Message Message::build(const vector<uint8_t> &payload, vector<cryptography::UUID> route,
                           cryptography::asymmetric::PublicKey destinationKey,
                           cryptography::asymmetric::KeyPair signer) {
        /// Sign the payload
        SIGNATURE_T signature(cryptography::asymmetric::sign(payload, signer.priv));

        /// Generate the shared secret and encrypt the payload
        vector<uint8_t> sharedSecret = cryptography::asymmetric::generateSharedSecret(destinationKey, signer.priv);
        // Note that since the IV is randomly generated its size can't mismatch so we can call unwrap
        vector<uint8_t> encryptedPayload = cryptography::symmetric::encrypt(payload, sharedSecret).unwrap();

        return Message(std::move(route), encryptedPayload, signature);
    }

    Result<Message, DeserializationError> Message::fromBuffer(vector<uint8_t> buffer) {

        using namespace scheme::communication;

        /// Verify the buffer type
        if (!flatbuffers::BufferHasIdentifier(buffer.data(), MessageDatagramIdentifier()))
            return Err(DeserializationError::INVALID_IDENTIFIER);

        /// Verify buffer integrity
        auto verifier = flatbuffers::Verifier(buffer.data(), buffer.size());
        if (!VerifyMessageDatagramBuffer(verifier))
            return Err(DeserializationError::INVALID_BUFFER);

        auto msg = GetMessageDatagram(buffer.data());

        /// Deserialize route
        vector<cryptography::UUID> route;
        auto routeBuffer = msg->route();
        for (uint i = 0; i < routeBuffer->Length(); i++)
            route.emplace_back(routeBuffer->Get(i));

        /// Deserialize payload
        vector<uint8_t> payload(msg->payload()->begin(), msg->payload()->end());

        /// Deserialize signature
        if (msg->signature()->size() != SIGNATURE_SIZE)
            return Err(DeserializationError::SIGNATURE_SIZE_MISMATCH);
        SIGNATURE_T signature{};
        // TODO Make this more memory efficient.
        copy(msg->signature()->begin(), msg->signature()->end(), signature.begin());

        return Ok(Message(route, payload, signature));
    }

#ifdef UNIT_TESTING

    SCENARIO("Message exchange and verification",
             "[unit_test][module][communication]") {

        GIVEN("A message") {
            cryptography::asymmetric::KeyPair keyPair(cryptography::asymmetric::generateKeyPair());
            cryptography::asymmetric::KeyPair destinationKeyPair(cryptography::asymmetric::generateKeyPair());
            cryptography::UUID origin;
            cryptography::UUID hop1;
            cryptography::UUID hop2;
            cryptography::UUID destination;

            vector<cryptography::UUID> route = {origin, hop1, hop2, destination};

            vector<uint8_t> payload = {1, 2, 3};
            Message msg = Message::build(payload, route, destinationKeyPair.pub, keyPair);

            WHEN("it is serialized") {
                vector<uint8_t> serializedMsg = msg.serialize();

                THEN("the serialized advertisement should contain an appropriate identifier") {
                    REQUIRE(flatbuffers::BufferHasIdentifier(serializedMsg.data(),
                                                             scheme::communication::MessageDatagramIdentifier()));
                }

                AND_WHEN("it is deserialized") {
                    Message deserializedMsg = Message::fromBuffer(serializedMsg).unwrap();

                    THEN("the payload should be encrypted") {
                        REQUIRE(deserializedMsg.payload != payload);

                        AND_WHEN("the payload is decrypted with the correct public key") {
                            vector<uint8_t> decryptedPayload = deserializedMsg.decryptPayload(keyPair.pub, destinationKeyPair).unwrap();
                            THEN("the decrypted payload should match the original one") {
                                REQUIRE(decryptedPayload == payload);
                            }
                        }

                        AND_WHEN("the payload is decrypted with the wrong public key") {
                            auto decryptedPayload = deserializedMsg.decryptPayload(destinationKeyPair.pub, keyPair);
                            THEN("the decrypted payload should throw an InvalidSignature error") {
                                REQUIRE(decryptedPayload.isErr());
                                REQUIRE(decryptedPayload.unwrapErr() == Message::MessageDecryptionError::InvalidSignature);
                            }
                        }
                    }

                    AND_WHEN("it is reserialized again") {
                        vector<uint8_t> reSerializedMsg = deserializedMsg.serialize();
                        THEN("both bytestreams should be equal") {
                            REQUIRE(reSerializedMsg == serializedMsg);
                        }
                    }
                }

                // TODO Write a test and function to check encryption and signature
            }
        }
    }

#endif
}