FROM christimperley/repairbox:ubuntu64

# Install necessary packages
RUN sudo apt-get update && \
    sudo apt-get install -y psmisc moreutils && \
    sudo apt-get clean && \
    sudo apt-get autoremove && \
    sudo rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

ADD problem.template.json /experiment/problem.template.json
ADD extract-preprocessed /usr/bin/extract-preprocessed
ADD test.sh /experiment/test.sh
ADD compile.sh /experiment/compile.sh
ADD generate-oracle /usr/bin/generate-oracle
ADD generate-description /usr/bin/generate-description
