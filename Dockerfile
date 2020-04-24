# Download Ubuntu 20.04
FROM ubuntu:20.04

# Copy the executable into the container
COPY build/freelistTest /home/

# Run the executable
CMD /home/freelistTest
